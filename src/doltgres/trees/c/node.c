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
 * Tree nodes (trees' node.rs). After the chunk header, a node's body is its
 * level (for an internal node only), its entry count, its key width (0 when
 * keys vary in length), its keys, and its values: for a leaf an offset per
 * value and the value bytes, for an internal node each child's address and
 * then each child's subtree entry count. A leaf's keys are front coded; an
 * internal node's are an offset per key and the key bytes, or fixed-width
 * keys back to back. Decoding lays a node out flat, which Node reads in
 * place. shared_prefix is proved by Verus against shared_len; the C's
 * guarantee is tested equivalence.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/header.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#ifdef DOLTGRES_TEST_TREES
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#endif
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/node.h"

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "node.rs"

/*
 * TREES_NODE_HEADER is HEADER, the length of a flat node's header: the
 * node's address, its level, its flags, then as four-byte little-endian
 * numbers its entry count, its body's offset, its key width, the offsets of
 * its keys, key bytes, values, value bytes, and summaries, and the lengths of
 * its bytes, its subtree counts, its address table, its locators, and its
 * stamps.
 */
#define TREES_NODE_HEADER ((size_t) (34 + 4 * 13))

/* TREES_NODE_BLOB is BLOB, the flat header's flag of a blob leaf. */
#define TREES_NODE_BLOB ((uint8) 1)

/* TREES_NODE_BY_COMPARATOR is BY_COMPARATOR, the flat header's flag of a node whose tree a comparator orders. */
#define TREES_NODE_BY_COMPARATOR ((uint8) 2)

/* TREES_NODE_SUMMARIES is SUMMARIES, the header flag of an internal node that carries a summary of each child's subtree. */
#define TREES_NODE_SUMMARIES ((uint8) 1)

/* TREES_NODE_COMPARATOR is COMPARATOR, the header flag of a node of a tree whose keys a comparator orders. */
#define TREES_NODE_COMPARATOR ((uint8) 2)

/*
 * TreesNodeDecoded is Decoded: a tree node as decoding makes it, before it
 * is laid out flat: the node's hashed bytes, with a leaf's keys written out
 * whole and keys that vary in length after a four-byte offset per key, and
 * what the layout's header holds.
 */
typedef struct TreesNodeDecoded
{
	StdVec		bytes;
	uint8		level;
	size_t		count;
	/* body is the offset of the body, after the chunk header. */
	size_t		body;
	size_t		key_width;
	size_t		keys_at;
	size_t		key_data_at;
	size_t		values_at;
	size_t		value_data_at;

	/*
	 * prefix is, for an internal node, the entry count of the subtrees before
	 * each child and after the last, a Vec<u64>.
	 */
	StdVec		prefix;

	/*
	 * summaries_at is, for an internal node with summaries, where their
	 * offsets start, or 0.
	 */
	size_t		summaries_at;

	/*
	 * table is, for a leaf, the body offsets of the addresses its values
	 * hold, in increasing order, a Vec<u32>.
	 */
	StdVec		table;
	/* blob is whether the node is a blob leaf, whose body is raw bytes. */
	bool		blob;

	/*
	 * comparator is whether the node's tree orders its keys by a comparator
	 * rather than by their bytes.
	 */
	bool		comparator;
} TreesNodeDecoded;

/* trees_node_Decoded_drop is Decoded's drop glue: its bytes', its subtree counts', and its address table's. */
static void
trees_node_Decoded_drop(TreesNodeDecoded *self)
{
	std_vec_free_Checked(&self->bytes, 1, 1);
	std_vec_free_Checked(&self->prefix, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&self->table, sizeof(uint32), _Alignof(uint32));
}

/* trees_node_u16_at reads a little-endian u16. */
static size_t
trees_node_u16_at(const uint8 *bytes, size_t bytes_len, size_t at)
{
	uint8		b[2];
	size_t		next;

	std_index_check(at, bytes_len, RUST_FILE, 114, 25);
	b[0] = bytes[at];
	next = STD_ADD(usize, at, 1, 114, 42);
	std_index_check(next, bytes_len, RUST_FILE, 114, 36);
	b[1] = bytes[next];
	return (size_t) std_u16_from_le_bytes(b);
}

/* trees_node_u32_at reads a little-endian u32. */
static size_t
trees_node_u32_at(const uint8 *bytes, size_t bytes_len, size_t at)
{
	size_t		end = STD_ADD(usize, at, 4, 119, 34);

	std_range_check(at, end, bytes_len, RUST_FILE, 119, 29);
	if (std_wrapping_sub_usize(end, at) != 4)
		STD_RESULT_EXPECT_FAILED(119, 53, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	return (size_t) std_u32_from_le_bytes(STD_PTR_AT(bytes, at, 1));
}

/* trees_node_ascending reports whether the n + 1 little-endian u16 offsets at the position never decrease. */
static bool
trees_node_ascending(const uint8 *bytes, size_t bytes_len, size_t at, size_t n)
{
	for (size_t i = 0, i_end = n; i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		size_t		twice = STD_MUL(usize, 2, i, 124, 39);
		size_t		left = trees_node_u16_at(bytes, bytes_len, STD_ADD(usize, at, twice, 124, 34));
		size_t		twice_next = STD_MUL(usize, 2, i, 124, 68);
		size_t		next = STD_ADD(usize, at, twice_next, 124, 63);
		size_t		right = trees_node_u16_at(bytes, bytes_len, STD_ADD(usize, next, 2, 124, 63));

		if (!(left <= right))
			return false;
	}
	return true;
}

/* trees_node_shared_prefix returns the length of the prefix two keys share. */
size_t
trees_node_shared_prefix(const uint8 *a, size_t a_len, const uint8 *b, size_t b_len)
{
	size_t		i = 0;

	for (;;)
	{
		if (!(i < a_len && i < b_len))
			break;
		std_index_check(i, a_len, RUST_FILE, 162, 41);
		std_index_check(i, b_len, RUST_FILE, 162, 49);
		if (!(a[i] == b[i]))
			break;
		i = STD_ADD(usize, i, 1, 169, 9);
	}
	return i;
}

/*
 * trees_node_next_key reads a front-coded leaf key's shared prefix length and
 * remaining length, moving past them, and checks that it shares no more than
 * the key before it and that its remaining bytes are there.
 */
static bool
trees_node_next_key(const uint8 *hashed, size_t hashed_len, size_t *at, size_t width, size_t before,
					size_t *result_0, size_t *result_1)
{
	uint64		value;
	ChunksVarintMalformed malformed;
	size_t		shared;
	size_t		rest;

	if (!chunks_varint_decode(hashed, hashed_len, at, &value, &malformed))
		return false;
	shared = std_usize_try_from_u64(value);
	switch (width)
	{
		case 0:
			if (!chunks_varint_decode(hashed, hashed_len, at, &value, &malformed))
				return false;
			rest = std_usize_try_from_u64(value);
			break;
		default:
			if (!std_checked_sub_usize(width, shared, &rest))
				return false;
			break;
	}
	if (!(shared <= before && rest <= STD_SUB(usize, hashed_len, *at, 187, 34)))
		return false;
	*result_0 = shared;
	*result_1 = rest;
	return true;
}

/*
 * trees_node_wide_offsets_frame_0_t is wide_offsets' state across its pad:
 * its arguments, its results, and the bytes it builds.
 */
typedef struct trees_node_wide_offsets_frame_0_t
{
	const uint8 *hashed;
	size_t		hashed_len;
	size_t		keys_at;
	size_t		count;
	StdVec		bytes;
	bool		bytes_live;
	StdVec	   *result_0;
	size_t	   *result_1;
	size_t	   *result_2;
	int64	   *result_3;
	bool		some;
} trees_node_wide_offsets_frame_0_t;

/* trees_node_wide_offsets_body_0 is wide_offsets' body. */
static void
trees_node_wide_offsets_body_0(void *frame_)
{
	trees_node_wide_offsets_frame_0_t *frame = frame_;
	const uint8 *hashed = frame->hashed;
	size_t		hashed_len = frame->hashed_len;
	size_t		keys_at = frame->keys_at;
	size_t		count = frame->count;
	size_t		data_at;
	size_t		moved;
	size_t		key_data_at;
	size_t		values_at;

	{
		size_t		n = STD_ADD(usize, count, 1, 194, 33);
		size_t		twice = STD_MUL(usize, 2, n, 194, 29);

		data_at = STD_ADD(usize, keys_at, twice, 194, 19);
	}
	if (data_at > hashed_len || !trees_node_ascending(hashed, hashed_len, keys_at, count))
	{
		frame->some = false;
		return;
	}
	{
		size_t		n = STD_ADD(usize, count, 1, 198, 59);
		size_t		twice = STD_MUL(usize, 2, n, 198, 55);
		size_t		capacity = STD_ADD(usize, hashed_len, twice, 198, 40);

		std_vec_with_capacity_Checked(&frame->bytes, capacity, 1, 1);
		frame->bytes_live = true;
	}
	std_range_to_check(keys_at, hashed_len, RUST_FILE, 199, 36);
	std_vec_extend_from_slice_Checked(&frame->bytes, hashed, keys_at, 1, 1);
	{
		size_t		i = 0,
					i_end = count;
		bool		i_done = i > i_end;

		for (; !i_done; i_done = (i == i_end), i = i_done ? i : std_forward_unchecked_usize_Checked(i, 1))
		{
			size_t		twice = STD_MUL(usize, 2, i, 200, 81);
			size_t		at = STD_ADD(usize, keys_at, twice, 200, 71);
			uint8		le[4];

			std_u32_to_le_bytes((uint32) trees_node_u16_at(hashed, hashed_len, at), le);
			std_vec_extend_from_slice_Checked(&frame->bytes, le, 4, 1, 1);
		}
	}
	moved = STD_SUB(usize, frame->bytes.len, data_at, 201, 17);
	std_range_from_check(data_at, hashed_len, RUST_FILE, 202, 36);
	std_vec_extend_from_slice_Checked(&frame->bytes, STD_PTR_AT(hashed, data_at, 1), std_wrapping_sub_usize(hashed_len, data_at), 1, 1);
	key_data_at = STD_ADD(usize, data_at, moved, 203, 23);
	{
		size_t		twice = STD_MUL(usize, 2, count, 204, 70);
		size_t		at = STD_ADD(usize, keys_at, twice, 204, 60);
		size_t		offset = trees_node_u16_at(hashed, hashed_len, at);

		values_at = STD_ADD(usize, key_data_at, offset, 204, 31);
	}
	frame->bytes_live = false;
	*frame->result_0 = frame->bytes;
	*frame->result_1 = key_data_at;
	*frame->result_2 = values_at;
	*frame->result_3 = (int64) moved;
	frame->some = true;
}

/* trees_node_wide_offsets_pad_0 is wide_offsets' pad: the bytes, while it holds them. */
static void
trees_node_wide_offsets_pad_0(void *frame_)
{
	trees_node_wide_offsets_frame_0_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * trees_node_wide_offsets returns an internal node's hashed bytes with the
 * offsets of its keys starting at keys_at widened to four bytes, as a leaf's
 * whole keys have them, with where the keys start, where the values start,
 * and how far the values moved, or none for offsets that run past the bytes
 * or decrease.
 */
static bool
trees_node_wide_offsets(const uint8 *hashed, size_t hashed_len, size_t keys_at, size_t count, StdVec *result_0,
						size_t *result_1, size_t *result_2, int64 *result_3)
{
	trees_node_wide_offsets_frame_0_t frame = {.hashed = hashed, .hashed_len = hashed_len, .keys_at = keys_at,
	.count = count, .result_0 = result_0, .result_1 = result_1, .result_2 = result_2, .result_3 = result_3};

	std_landing(&frame, trees_node_wide_offsets_body_0, trees_node_wide_offsets_pad_0);
	return frame.some;
}

/*
 * trees_node_whole_keys_frame_0_t is whole_keys' state across its pad: its
 * arguments, its results, and the bytes it builds.
 */
typedef struct trees_node_whole_keys_frame_0_t
{
	const uint8 *hashed;
	size_t		hashed_len;
	size_t		keys_at;
	size_t		count;
	size_t		width;
	StdVec		bytes;
	bool		bytes_live;
	StdVec	   *result_0;
	size_t	   *result_1;
	size_t	   *result_2;
	int64	   *result_3;
	bool		some;
} trees_node_whole_keys_frame_0_t;

/* trees_node_whole_keys_body_0 is whole_keys' body. */
static void
trees_node_whole_keys_body_0(void *frame_)
{
	trees_node_whole_keys_frame_0_t *frame = frame_;
	const uint8 *hashed = frame->hashed;
	size_t		hashed_len = frame->hashed_len;
	size_t		keys_at = frame->keys_at;
	size_t		count = frame->count;
	size_t		width = frame->width;
	size_t		at = keys_at;
	size_t		before = 0;
	size_t		total = 0;
	size_t		offsets;
	size_t		data_at;
	size_t		at_2;
	size_t		previous;
	size_t		before_2;
	size_t		values_at;
	int64		moved;

	for (size_t n = 0, n_end = count; n < n_end; n = std_forward_unchecked_usize_Checked(n, 1))
	{
		size_t		shared;
		size_t		rest;

		if (!trees_node_next_key(hashed, hashed_len, &at, width, before, &shared, &rest))
		{
			frame->some = false;
			return;
		}
		at = STD_ADD(usize, at, rest, 214, 9);
		before = STD_ADD(usize, shared, rest, 215, 18);
		total = STD_ADD(usize, total, before, 216, 9);
	}
	if (width == 0)
	{
		size_t		n = STD_ADD(usize, count, 1, 218, 39);

		offsets = STD_MUL(usize, 4, n, 218, 35);
	}
	else
		offsets = 0;
	{
		size_t		capacity = STD_ADD(usize, keys_at, offsets, 219, 40);

		capacity = STD_ADD(usize, capacity, total, 219, 40);
		capacity = STD_ADD(usize, capacity, hashed_len, 219, 40);
		capacity = STD_SUB(usize, capacity, at, 219, 40);
		std_vec_with_capacity_Checked(&frame->bytes, capacity, 1, 1);
		frame->bytes_live = true;
	}
	std_range_to_check(keys_at, hashed_len, RUST_FILE, 220, 36);
	std_vec_extend_from_slice_Checked(&frame->bytes, hashed, keys_at, 1, 1);
	{
		uint8		zero = 0;

		std_vec_resize(&frame->bytes, STD_ADD(usize, keys_at, offsets, 221, 18), &zero, 1, 1);
	}
	data_at = frame->bytes.len;
	at_2 = keys_at;
	previous = data_at;
	before_2 = 0;
	for (size_t i = 0, i_end = count; i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		size_t		shared;
		size_t		rest;
		size_t		start;

		if (!trees_node_next_key(hashed, hashed_len, &at_2, width, before_2, &shared, &rest))
		{
			frame->bytes_live = false;
			std_vec_free_Checked(&frame->bytes, 1, 1);
			frame->some = false;
			return;
		}
		start = frame->bytes.len;
		if (width == 0)
		{
			size_t		four = STD_MUL(usize, 4, i, 228, 29);
			size_t		from = STD_ADD(usize, keys_at, four, 228, 19);
			size_t		four_end = STD_MUL(usize, 4, i, 228, 46);
			size_t		to = STD_ADD(usize, keys_at, four_end, 228, 36);
			uint8		le[4];

			to = STD_ADD(usize, to, 4, 228, 36);
			std_range_check(from, to, frame->bytes.len, RUST_FILE, 228, 18);
			std_u32_to_le_bytes((uint32) STD_SUB(usize, start, data_at, 228, 75), le);
			std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->bytes.ptr, from, 1), std_wrapping_sub_usize(to, from), le, 4, 1, 1, RUST_FILE,
											  228, 57);
		}
		std_vec_extend_from_within_Checked(&frame->bytes, previous,
										   STD_ADD(usize, previous, shared, 230, 44), 1, 1);
		{
			size_t		end = STD_ADD(usize, at_2, rest, 231, 45);

			std_range_check(at_2, end, hashed_len, RUST_FILE, 231, 40);
			std_vec_extend_from_slice_Checked(&frame->bytes, STD_PTR_AT(hashed, at_2, 1), std_wrapping_sub_usize(end, at_2), 1, 1);
		}
		{
			size_t		next_at = STD_ADD(usize, at_2, rest, 232, 35);
			size_t		next_before = STD_ADD(usize, shared, rest, 232, 53);

			at_2 = next_at;
			previous = start;
			before_2 = next_before;
		}
	}
	if (width == 0)
	{
		size_t		four = STD_MUL(usize, 4, count, 235, 25);
		size_t		from = STD_ADD(usize, keys_at, four, 235, 15);
		size_t		four_end = STD_MUL(usize, 4, count, 235, 46);
		size_t		to = STD_ADD(usize, keys_at, four_end, 235, 36);
		uint8		le[4];

		to = STD_ADD(usize, to, 4, 235, 36);
		std_range_check(from, to, frame->bytes.len, RUST_FILE, 235, 14);
		std_u32_to_le_bytes((uint32) total, le);
		std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->bytes.ptr, from, 1), std_wrapping_sub_usize(to, from), le, 4, 1, 1, RUST_FILE,
										  235, 61);
	}
	values_at = frame->bytes.len;
	std_range_from_check(at_2, hashed_len, RUST_FILE, 238, 36);
	std_vec_extend_from_slice_Checked(&frame->bytes, STD_PTR_AT(hashed, at_2, 1), std_wrapping_sub_usize(hashed_len, at_2), 1, 1);
	moved = STD_SUB(i64, (int64) values_at, (int64) at_2, 239, 38);
	frame->bytes_live = false;
	*frame->result_0 = frame->bytes;
	*frame->result_1 = data_at;
	*frame->result_2 = values_at;
	*frame->result_3 = moved;
	frame->some = true;
}

/* trees_node_whole_keys_pad_0 is whole_keys' pad: the bytes, while it holds them. */
static void
trees_node_whole_keys_pad_0(void *frame_)
{
	trees_node_whole_keys_frame_0_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * trees_node_whole_keys returns a leaf's hashed bytes with the front-coded
 * keys starting at keys_at written out whole, after an offset per key and
 * the end when they vary in length, with where the whole keys start, where
 * the values start, and how far the values moved, or none for keys that run
 * past the bytes.
 */
static bool
trees_node_whole_keys(const uint8 *hashed, size_t hashed_len, size_t keys_at, size_t count, size_t width,
					  StdVec *result_0, size_t *result_1, size_t *result_2, int64 *result_3)
{
	trees_node_whole_keys_frame_0_t frame = {.hashed = hashed, .hashed_len = hashed_len, .keys_at = keys_at,
		.count = count, .width = width, .result_0 = result_0, .result_1 = result_1, .result_2 = result_2,
	.result_3 = result_3};

	std_landing(&frame, trees_node_whole_keys_body_0, trees_node_whole_keys_pad_0);
	return frame.some;
}

/* trees_node_flatten_closure_0 is flatten's address count: the length of the address table of the hashed bytes' header. */
static bool
trees_node_flatten_closure_0(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
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

static bool trees_node_flat_of(ChunksAddressAddress address, const uint8 *hashed, size_t hashed_len,
							   const uint8 *locators, size_t locators_len, const uint8 *stamps, size_t stamps_len,
							   uint8 reads, StdVec *result, ChunksErrorError *error);

/* trees_node_flatten decodes a node from a chunk's stored bytes into its flat bytes, which Node's view reads. */
bool
trees_node_flatten(const ChunksChunkChunk *chunk, StdVec *result, ChunksErrorError *error)
{
	ChunksTrailerStored split;
	StdBytes	locators;
	StdBytes	stamps;
	uint8		reads;

	if (!chunks_chunk_Chunk_split(chunk, trees_node_flatten_closure_0, NULL, NULL, &split, error))
		return false;
	chunks_trailer_Stored_trailer(&split, &locators, &stamps, &reads);
	return trees_node_flat_of(chunks_chunk_Chunk_address(chunk), split.hashed.ptr, split.hashed.len, locators.ptr,
							  locators.len, stamps.ptr, stamps.len, reads, result, error);
}

static bool trees_node_Decoded_of(ChunksAddressAddress address, const uint8 *hashed, size_t hashed_len,
								  TreesNodeDecoded *result, ChunksErrorError *error);

/*
 * trees_node_flat_of_frame_0_t is flat_of's state across its pad: its
 * arguments, its results, the decoded node, and the flat bytes it builds.
 */
typedef struct trees_node_flat_of_frame_0_t
{
	ChunksAddressAddress address;
	const uint8 *hashed;
	size_t		hashed_len;
	const uint8 *locators;
	size_t		locators_len;
	const uint8 *stamps;
	size_t		stamps_len;
	uint8		reads;
	TreesNodeDecoded decoded;
	bool		decoded_live;
	StdVec		flat;
	bool		flat_live;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
} trees_node_flat_of_frame_0_t;

/* trees_node_flat_of_body_0 is flat_of's body. */
static void
trees_node_flat_of_body_0(void *frame_)
{
	trees_node_flat_of_frame_0_t *frame = frame_;
	size_t		parts[5];
	size_t		offsets[8];
	size_t		len;

	if (!trees_node_Decoded_of(frame->address, frame->hashed, frame->hashed_len, &frame->decoded, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->decoded_live = true;
	parts[0] = frame->decoded.bytes.len;
	parts[1] = frame->decoded.prefix.len;
	parts[2] = frame->decoded.table.len;
	parts[3] = frame->locators_len;
	parts[4] = frame->stamps_len;
	len = STD_ADD(usize, TREES_NODE_HEADER, parts[0], 253, 15);
	len = STD_ADD(usize, len, STD_MUL(usize, 8, parts[1], 253, 35), 253, 15);
	len = STD_ADD(usize, len, STD_MUL(usize, 4, parts[2], 253, 50), 253, 15);
	len = STD_ADD(usize, len, parts[3], 253, 15);
	len = STD_ADD(usize, len, parts[4], 253, 15);
	std_vec_with_capacity_Checked(&frame->flat, len, 1, 1);
	frame->flat_live = true;
	std_vec_extend_from_slice_Checked(&frame->flat, frame->address.f0, CHUNKS_ADDRESS_ADDRESS_LEN, 1, 1);
	std_vec_u8_push_Checked(&frame->flat, frame->decoded.level);
	std_vec_u8_push_Checked(&frame->flat, (frame->decoded.blob ? TREES_NODE_BLOB : 0) |
							(frame->decoded.comparator ? TREES_NODE_BY_COMPARATOR : 0) |
							STD_SHL_FOLDED(u8, frame->reads, 2));
	offsets[0] = frame->decoded.count;
	offsets[1] = frame->decoded.body;
	offsets[2] = frame->decoded.key_width;
	offsets[3] = frame->decoded.keys_at;
	offsets[4] = frame->decoded.key_data_at;
	offsets[5] = frame->decoded.values_at;
	offsets[6] = frame->decoded.value_data_at;
	offsets[7] = frame->decoded.summaries_at;
	for (size_t k = 0, k_len = lengthof(offsets) + lengthof(parts); k < k_len; k++)
	{
		size_t		number = k < lengthof(offsets) ? offsets[k] : parts[std_wrapping_sub_usize(k, lengthof(offsets))];
		uint32		number_2;
		StdTryFromIntError error_;
		uint8		le[4];

		if (!std_u32_try_from_usize(number, &number_2, &error_))
		{
			StdString	message;
			static const StdFmtPiece pieces[] = {STD_FMT_LIT("tree node "), STD_FMT_ARG(0),
			STD_FMT_LIT(" is too large")};
			StdFmtArg	args[] = {{&frame->address, chunks_address_Address_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_fmt_format_Checked(&message, &arguments);
			*frame->error = chunks_error_Error_corrupt(&message);
			frame->flat_live = false;
			std_vec_free_Checked(&frame->flat, 1, 1);
			frame->decoded_live = false;
			trees_node_Decoded_drop(&frame->decoded);
			frame->ok = false;
			return;
		}
		std_u32_to_le_bytes(number_2, le);
		std_vec_extend_from_slice_Checked(&frame->flat, le, 4, 1, 1);
	}
	std_vec_extend_from_slice_Checked(&frame->flat, frame->decoded.bytes.ptr, frame->decoded.bytes.len, 1, 1);
	for (size_t k = 0, k_len = frame->decoded.prefix.len; k < k_len; k++)
	{
		uint8		le[8];

		std_u64_to_le_bytes(STD_VEC_AT(&frame->decoded.prefix, uint64, k), le);
		std_vec_extend_from_slice_Checked(&frame->flat, le, 8, 1, 1);
	}
	for (size_t k = 0, k_len = frame->decoded.table.len; k < k_len; k++)
	{
		uint8		le[4];

		std_u32_to_le_bytes(STD_VEC_AT(&frame->decoded.table, uint32, k), le);
		std_vec_extend_from_slice_Checked(&frame->flat, le, 4, 1, 1);
	}
	std_vec_extend_from_slice_Checked(&frame->flat, frame->locators, frame->locators_len, 1, 1);
	std_vec_extend_from_slice_Checked(&frame->flat, frame->stamps, frame->stamps_len, 1, 1);
	frame->flat_live = false;
	*frame->result = frame->flat;
	frame->decoded_live = false;
	trees_node_Decoded_drop(&frame->decoded);
	frame->ok = true;
}

/* trees_node_flat_of_pad_0 is flat_of's pad: the flat bytes and the decoded node, while it holds them. */
static void
trees_node_flat_of_pad_0(void *frame_)
{
	trees_node_flat_of_frame_0_t *frame = frame_;

	if (frame->flat_live)
		std_vec_free_Checked(&frame->flat, 1, 1);
	if (frame->decoded_live)
		trees_node_Decoded_drop(&frame->decoded);
}

/* trees_node_flat_of lays a node out flat from its hashed bytes and its trailer's locators and stamps. */
static bool
trees_node_flat_of(ChunksAddressAddress address, const uint8 *hashed, size_t hashed_len, const uint8 *locators,
				   size_t locators_len, const uint8 *stamps, size_t stamps_len, uint8 reads, StdVec *result,
				   ChunksErrorError *error)
{
	trees_node_flat_of_frame_0_t frame = {.address = address, .hashed = hashed, .hashed_len = hashed_len,
		.locators = locators, .locators_len = locators_len, .stamps = stamps, .stamps_len = stamps_len,
	.reads = reads, .result = result, .error = error};

	std_landing(&frame, trees_node_flat_of_body_0, trees_node_flat_of_pad_0);
	return frame.ok;
}

/* trees_node_Decoded_of_closure_0 is of's corrupt: the error of a node whose bytes are corrupt. */
static ChunksErrorError
trees_node_Decoded_of_closure_0(void *captures)
{
	StdString	message;
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("tree node "), STD_FMT_ARG(0), STD_FMT_LIT(" is corrupt")};
	StdFmtArg	args[] = {{captures, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&message, &arguments);
	return chunks_error_Error_corrupt(&message);
}

/*
 * trees_node_Decoded_of_frame_0_t is of's state across its pad: its
 * arguments, its result, and the header, address offsets, bytes, subtree
 * counts, and address table it holds.
 */
typedef struct trees_node_Decoded_of_frame_0_t
{
	ChunksAddressAddress address;
	const uint8 *hashed;
	size_t		hashed_len;
	ChunksHeaderHeader header;
	bool		header_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		prefix;
	bool		prefix_live;

	/*
	 * offsets is the header's address offsets, moved out of it to be
	 * collected in place into the address table.
	 */
	StdVec		offsets;
	bool		offsets_live;
	StdVec		table;
	bool		table_live;
	TreesNodeDecoded *result;
	ChunksErrorError *error;
	bool		ok;
} trees_node_Decoded_of_frame_0_t;

/* trees_node_Decoded_of_body_0 is of's body. */
static void
trees_node_Decoded_of_body_0(void *frame_)
{
	trees_node_Decoded_of_frame_0_t *frame = frame_;
	const uint8 *hashed = frame->hashed;
	size_t		hashed_len = frame->hashed_len;
	const uint8 *hashed_2;
	size_t		hashed_2_len;
	size_t		body;
	bool		leaf;
	size_t		at;
	uint8		level;
	size_t		count;
	size_t		key_width;
	size_t		keys_at;
	size_t		key_data_at;
	size_t		values_at;
	int64		moved;
	size_t		value_data_at_2;
	size_t		end;
	size_t		at_2;
	size_t		summaries_at = 0;
	int64		values_start;
	int64		values_end;

	if (!chunks_header_Header_read(hashed, hashed_len, &frame->header, &body, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->header_live = true;
	if (frame->header.kind == CHUNKS_HEADER_BLOB_LEAF)
	{
		TreesNodeDecoded decoded = {.body = body, .blob = true};

		std_slice_to_vec_Checked(&decoded.bytes, hashed, hashed_len, 1, 1);
		std_vec_new(&decoded.prefix, sizeof(uint64), _Alignof(uint64));
		std_vec_new(&decoded.table, sizeof(uint32), _Alignof(uint32));
		*frame->result = decoded;
		frame->header_live = false;
		chunks_header_Header_drop(&frame->header);
		frame->ok = true;
		return;
	}
	switch (frame->header.kind)
	{
		case CHUNKS_HEADER_TREE_LEAF:
			leaf = true;
			break;
		case CHUNKS_HEADER_TREE_INTERNAL:
			leaf = false;
			break;
		default:
			*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
			frame->header_live = false;
			chunks_header_Header_drop(&frame->header);
			frame->ok = false;
			return;
	}
	at = body;
	if (leaf)
		level = 0;
	else
	{
		size_t		before;

		at = STD_ADD(usize, at, 1, 312, 17);
		before = STD_SUB(usize, at, 1, 313, 29);
		if (!(before < hashed_len))
		{
			*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
			frame->header_live = false;
			chunks_header_Header_drop(&frame->header);
			frame->ok = false;
			return;
		}
		level = hashed[before];
	}
	{
		uint64		value;

		if (!chunks_varint_read_varint(hashed, hashed_len, &at, &value, frame->error))
		{
			frame->header_live = false;
			chunks_header_Header_drop(&frame->header);
			frame->ok = false;
			return;
		}
		count = (size_t) value;
	}
	if (!(at < hashed_len))
	{
		*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
		frame->header_live = false;
		chunks_header_Header_drop(&frame->header);
		frame->ok = false;
		return;
	}
	key_width = (size_t) hashed[at];
	at = STD_ADD(usize, at, 1, 318, 9);
	if (leaf != (level == 0) || count > hashed_len)
	{
		*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
		frame->header_live = false;
		chunks_header_Header_drop(&frame->header);
		frame->ok = false;
		return;
	}
	keys_at = at;
	if (leaf)
	{
		if (!trees_node_whole_keys(hashed, hashed_len, keys_at, count, key_width, &frame->bytes, &key_data_at,
								   &values_at, &moved))
		{
			*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
			frame->header_live = false;
			chunks_header_Header_drop(&frame->header);
			frame->ok = false;
			return;
		}
		frame->bytes_live = true;
	}
	else if (key_width == 0)
	{
		if (!trees_node_wide_offsets(hashed, hashed_len, keys_at, count, &frame->bytes, &key_data_at, &values_at,
									 &moved))
		{
			*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
			frame->header_live = false;
			chunks_header_Header_drop(&frame->header);
			frame->ok = false;
			return;
		}
		frame->bytes_live = true;
	}
	else
	{
		std_slice_to_vec_Checked(&frame->bytes, hashed, hashed_len, 1, 1);
		frame->bytes_live = true;
		key_data_at = keys_at;
		values_at = STD_ADD(usize, keys_at, STD_MUL(usize, key_width, count, 326, 64),
							326, 54);
		moved = 0;
	}
	hashed_2 = frame->bytes.ptr;
	hashed_2_len = frame->bytes.len;
	if (leaf)
	{
		size_t		n = STD_ADD(usize, count, 1, 330, 49);
		size_t		twice = STD_MUL(usize, 2, n, 330, 45);
		size_t		value_data_at = STD_ADD(usize, values_at, twice, 330, 33);

		if (value_data_at > hashed_2_len || !trees_node_ascending(hashed_2, hashed_2_len, values_at, count))
		{
			*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
			frame->bytes_live = false;
			std_vec_free_Checked(&frame->bytes, 1, 1);
			frame->header_live = false;
			chunks_header_Header_drop(&frame->header);
			frame->ok = false;
			return;
		}
		{
			size_t		twice_count = STD_MUL(usize, 2, count, 334, 72);
			size_t		offset = trees_node_u16_at(hashed_2, hashed_2_len,
												   STD_ADD(usize, values_at, twice_count,
														   334, 60));

			value_data_at_2 = value_data_at;
			end = STD_ADD(usize, value_data_at, offset, 334, 29);
		}
	}
	else
	{
		value_data_at_2 = STD_ADD(usize, values_at, STD_MUL(usize, 32, count, 336, 26),
								  336, 14);
		end = STD_ADD(usize, values_at, STD_MUL(usize, 32, count, 336, 50),
					  336, 38);
	}
	if (end > hashed_2_len)
	{
		*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		frame->header_live = false;
		chunks_header_Header_drop(&frame->header);
		frame->ok = false;
		return;
	}
	std_vec_new(&frame->prefix, sizeof(uint64), _Alignof(uint64));
	frame->prefix_live = true;
	at_2 = end;
	if (!leaf)
	{
		uint64		total = 0;
		uint64		zero = 0;

		std_vec_reserve_Checked(&frame->prefix, STD_ADD(usize, count, 1, 344, 28), sizeof(uint64),
								_Alignof(uint64));
		std_vec_push_Checked(&frame->prefix, &zero, sizeof(uint64), _Alignof(uint64));
		for (size_t n = 0, n_end = count; n < n_end; n = std_forward_unchecked_usize_Checked(n, 1))
		{
			uint64		value;

			if (!chunks_varint_read_varint(hashed_2, hashed_2_len, &at_2, &value, frame->error))
			{
				frame->prefix_live = false;
				std_vec_free_Checked(&frame->prefix, sizeof(uint64), _Alignof(uint64));
				frame->bytes_live = false;
				std_vec_free_Checked(&frame->bytes, 1, 1);
				frame->header_live = false;
				chunks_header_Header_drop(&frame->header);
				frame->ok = false;
				return;
			}
			if (!std_checked_add_u64(total, value, &total))
			{
				*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
				frame->prefix_live = false;
				std_vec_free_Checked(&frame->prefix, sizeof(uint64), _Alignof(uint64));
				frame->bytes_live = false;
				std_vec_free_Checked(&frame->bytes, 1, 1);
				frame->header_live = false;
				chunks_header_Header_drop(&frame->header);
				frame->ok = false;
				return;
			}
			std_vec_push_Checked(&frame->prefix, &total, sizeof(uint64), _Alignof(uint64));
		}
	}
	if (!leaf && (frame->header.flags & TREES_NODE_SUMMARIES) != 0)
	{
		size_t		data_at;

		summaries_at = at_2;
		{
			size_t		n = STD_ADD(usize, count, 1, 355, 36);
			size_t		twice = STD_MUL(usize, 2, n, 355, 32);

			data_at = STD_ADD(usize, at_2, twice, 355, 27);
		}
		if (data_at > hashed_2_len || !trees_node_ascending(hashed_2, hashed_2_len, at_2, count))
		{
			*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
			frame->prefix_live = false;
			std_vec_free_Checked(&frame->prefix, sizeof(uint64), _Alignof(uint64));
			frame->bytes_live = false;
			std_vec_free_Checked(&frame->bytes, 1, 1);
			frame->header_live = false;
			chunks_header_Header_drop(&frame->header);
			frame->ok = false;
			return;
		}
		{
			size_t		twice_count = STD_MUL(usize, 2, count, 359, 48);
			size_t		offset = trees_node_u16_at(hashed_2, hashed_2_len,
												   STD_ADD(usize, at_2, twice_count, 359, 43));

			at_2 = STD_ADD(usize, data_at, offset, 359, 18);
		}
	}
	values_start = (int64) STD_SUB(usize, value_data_at_2, body, 361, 22);
	values_end = STD_SUB(i64, (int64) STD_SUB(usize, end, body, 361, 54), 32,
						 361, 54);
	if (frame->header.addresses.tag == CHUNKS_HEADER_ADDRESSES_OFFSETS && leaf)
	{
		size_t		len = 0;
		bool		some = true;

		frame->header_live = false;
		frame->offsets = frame->header.addresses.u.offsets;
		frame->offsets_live = true;
		for (size_t k = 0, k_len = frame->offsets.len; k < k_len; k++)
		{
			int64		offset = STD_ADD(i64, (int64) STD_VEC_AT(&frame->offsets, uint32, k), moved,
										 365, 36);

			if (!(values_start <= offset && offset <= values_end))
			{
				some = false;
				break;
			}
			STD_VEC_AT(&frame->offsets, uint32, len) = (uint32) offset;
			len = std_wrapping_add_usize(len, 1);
		}
		frame->offsets.len = len;
		frame->offsets_live = false;
		if (!some)
		{
			std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
			*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
			frame->prefix_live = false;
			std_vec_free_Checked(&frame->prefix, sizeof(uint64), _Alignof(uint64));
			frame->bytes_live = false;
			std_vec_free_Checked(&frame->bytes, 1, 1);
			frame->ok = false;
			return;
		}
		frame->table = frame->offsets;
	}
	else
		std_vec_new(&frame->table, sizeof(uint32), _Alignof(uint32));
	frame->table_live = true;
	{
		bool		corrupt = at_2 != hashed_2_len;

		if (!corrupt)
		{
			StdWindows	windows = std_slice_windows(frame->table.ptr, frame->table.len, 2, RUST_FILE,
													370, 40);
			const void *window;
			size_t		window_len;

			while (std_slice_Windows_next(&windows, sizeof(uint32), &window, &window_len))
			{
				const uint32 *w = window;
				uint32		left;

				std_index_check(0, window_len, RUST_FILE, 370, 59);
				left = STD_ADD(u32, w[0], 32, 370, 59);
				std_index_check(1, window_len, RUST_FILE, 370, 71);
				if (left > w[1])
				{
					corrupt = true;
					break;
				}
			}
		}
		if (corrupt)
		{
			*frame->error = trees_node_Decoded_of_closure_0(&frame->address);
			frame->table_live = false;
			std_vec_free_Checked(&frame->table, sizeof(uint32), _Alignof(uint32));
			frame->prefix_live = false;
			std_vec_free_Checked(&frame->prefix, sizeof(uint64), _Alignof(uint64));
			frame->bytes_live = false;
			std_vec_free_Checked(&frame->bytes, 1, 1);
			if (frame->header_live)
			{
				frame->header_live = false;
				chunks_header_Header_drop(&frame->header);
			}
			frame->ok = false;
			return;
		}
	}
	{
		TreesNodeDecoded decoded = {.level = level, .count = count, .body = body, .key_width = key_width,
			.keys_at = keys_at, .key_data_at = key_data_at, .values_at = values_at, .value_data_at = value_data_at_2,
		.summaries_at = summaries_at, .comparator = (frame->header.flags & TREES_NODE_COMPARATOR) != 0};

		frame->bytes_live = false;
		decoded.bytes = frame->bytes;
		frame->prefix_live = false;
		decoded.prefix = frame->prefix;
		frame->table_live = false;
		decoded.table = frame->table;
		*frame->result = decoded;
	}
	if (frame->header_live)
	{
		frame->header_live = false;
		chunks_header_Header_drop(&frame->header);
	}
	frame->ok = true;
}

/* trees_node_Decoded_of_pad_0 is of's pad: what it holds, in reverse order of declaration. */
static void
trees_node_Decoded_of_pad_0(void *frame_)
{
	trees_node_Decoded_of_frame_0_t *frame = frame_;

	if (frame->table_live)
		std_vec_free_Checked(&frame->table, sizeof(uint32), _Alignof(uint32));
	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	if (frame->prefix_live)
		std_vec_free_Checked(&frame->prefix, sizeof(uint64), _Alignof(uint64));
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->header_live)
		chunks_header_Header_drop(&frame->header);
}

/* trees_node_Decoded_of decodes a node from its hashed bytes. */
static bool
trees_node_Decoded_of(ChunksAddressAddress address, const uint8 *hashed, size_t hashed_len, TreesNodeDecoded *result,
					  ChunksErrorError *error)
{
	trees_node_Decoded_of_frame_0_t frame = {.address = address, .hashed = hashed, .hashed_len = hashed_len,
	.result = result, .error = error};

	std_landing(&frame, trees_node_Decoded_of_body_0, trees_node_Decoded_of_pad_0);
	return frame.ok;
}

/*
 * trees_node_Flat_of decodes the node a chunk holds, for a check of the files
 * that reads each chunk itself, checked against its address as every read
 * checks it, rather than through a store.
 */
bool
trees_node_Flat_of(const ChunksChunkChunk *chunk, TreesNodeFlat *result, ChunksErrorError *error)
{
	StdVec		flat;

	if (!trees_node_flatten(chunk, &flat, error))
		return false;
	result->f0.tag = HOST_CACHE_READ_UNCACHED;
	result->f0.u.uncached = flat;
	return true;
}

/* trees_node_Flat_node returns the node the bytes hold. */
TreesNodeNode
trees_node_Flat_node(const TreesNodeFlat *self)
{
	StdBytes	flat = host_cache_Read_Deref_deref(&self->f0);

	return trees_node_Node_view(flat.ptr, flat.len);
}

/* trees_node_Flat_drop is Flat's drop glue: its read's. */
void
trees_node_Flat_drop(TreesNodeFlat *self)
{
	host_cache_Read_drop(&self->f0);
}

/* trees_node_Node_Debug_fmt is Node's derived Debug. */
bool
trees_node_Node_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesNodeNode *self = (const TreesNodeNode *) value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Node", 4);

	std_DebugStruct_field(&s, "address", 7, &self->address, chunks_address_Address_Debug_fmt);
	std_DebugStruct_field(&s, "bytes", 5, &self->bytes, std_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "level", 5, &self->level, std_u8_Debug_fmt);
	std_DebugStruct_field(&s, "count", 5, &self->count, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "body", 4, &self->body, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "key_width", 9, &self->key_width, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "keys_at", 7, &self->keys_at, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "key_data_at", 11, &self->key_data_at, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "values_at", 9, &self->values_at, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "value_data_at", 13, &self->value_data_at, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "prefix", 6, &self->prefix, std_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "summaries_at", 12, &self->summaries_at, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "table", 5, &self->table, std_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "blob", 4, &self->blob, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "comparator", 10, &self->comparator, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "locators", 8, &self->locators, std_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "stamps", 6, &self->stamps, std_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "reads", 5, &self->reads, std_u8_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* trees_node_Node_view_closure_0 is view's number: the four-byte number at the position in the flat header. */
static size_t
trees_node_Node_view_closure_0(void *captures, size_t i)
{
	const StdBytes *flat = captures;
	size_t		four = STD_MUL(usize, 4, i, 414, 51);

	return trees_node_u32_at(flat->ptr, flat->len, STD_ADD(usize, 34, four, 414, 46));
}

/* trees_node_Node_view returns the node flat bytes hold. */
TreesNodeNode
trees_node_Node_view(const uint8 *flat, size_t flat_len)
{
	StdBytes	captures = {flat, flat_len};
	TreesNodeNode node;
	const void *bytes;
	size_t		bytes_len;
	const void *prefix;
	size_t		prefix_len;
	const void *table;
	size_t		table_len;
	const void *locators;
	size_t		locators_len;
	const void *rest;
	size_t		rest_len;
	const void *rest_2;
	size_t		rest_2_len;
	const void *rest_3;
	size_t		rest_3_len;
	const void *rest_4;
	size_t		rest_4_len;
	size_t		mid;

	std_range_from_check(TREES_NODE_HEADER, flat_len, RUST_FILE, 415, 33);
	mid = trees_node_Node_view_closure_0(&captures, 8);
	std_slice_split_at_Checked(STD_PTR_AT(flat, TREES_NODE_HEADER, 1), std_wrapping_sub_usize(flat_len, TREES_NODE_HEADER), mid, 1, 1, &bytes, &bytes_len,
							   &rest, &rest_len, RUST_FILE, 415, 44);
	mid = STD_MUL(usize, 8, trees_node_Node_view_closure_0(&captures, 9), 416, 44);
	std_slice_split_at_Checked(rest, rest_len, mid, 1, 1, &prefix, &prefix_len, &rest_2, &rest_2_len, RUST_FILE,
							   416, 35);
	mid = STD_MUL(usize, 4, trees_node_Node_view_closure_0(&captures, 10), 417, 43);
	std_slice_split_at_Checked(rest_2, rest_2_len, mid, 1, 1, &table, &table_len, &rest_3, &rest_3_len, RUST_FILE,
							   417, 34);
	mid = trees_node_Node_view_closure_0(&captures, 11);
	std_slice_split_at_Checked(rest_3, rest_3_len, mid, 1, 1, &locators, &locators_len, &rest_4, &rest_4_len, RUST_FILE,
							   418, 37);
	std_range_to_check(32, flat_len, RUST_FILE, 420, 34);
	if (32 != CHUNKS_ADDRESS_ADDRESS_LEN)
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt, RUST_FILE, 420, 52);
	memcpy(node.address.f0, flat, CHUNKS_ADDRESS_ADDRESS_LEN);
	node.bytes.ptr = bytes;
	node.bytes.len = bytes_len;
	std_index_check(32, flat_len, RUST_FILE, 422, 20);
	node.level = flat[32];
	node.count = trees_node_Node_view_closure_0(&captures, 0);
	node.body = trees_node_Node_view_closure_0(&captures, 1);
	node.key_width = trees_node_Node_view_closure_0(&captures, 2);
	node.keys_at = trees_node_Node_view_closure_0(&captures, 3);
	node.key_data_at = trees_node_Node_view_closure_0(&captures, 4);
	node.values_at = trees_node_Node_view_closure_0(&captures, 5);
	node.value_data_at = trees_node_Node_view_closure_0(&captures, 6);
	node.prefix.ptr = prefix;
	node.prefix.len = prefix_len;
	node.summaries_at = trees_node_Node_view_closure_0(&captures, 7);
	node.table.ptr = table;
	node.table.len = table_len;
	std_index_check(33, flat_len, RUST_FILE, 433, 19);
	node.blob = (flat[33] & TREES_NODE_BLOB) != 0;
	std_index_check(33, flat_len, RUST_FILE, 434, 25);
	node.comparator = (flat[33] & TREES_NODE_BY_COMPARATOR) != 0;
	node.locators.ptr = locators;
	node.locators.len = locators_len;
	mid = trees_node_Node_view_closure_0(&captures, 12);
	std_range_to_check(mid, rest_4_len, RUST_FILE, 436, 26);
	node.stamps.ptr = rest_4;
	node.stamps.len = mid;
	std_index_check(33, flat_len, RUST_FILE, 437, 20);
	node.reads = STD_SHR_FOLDED(u8, flat[33], 2);
	return node;
}

/* trees_node_Node_stamps returns the stamps of the node's entries, from its trailer. */
bool
trees_node_Node_stamps(const TreesNodeNode *self, ChunksTrailerStamps *result, ChunksErrorError *error)
{
	StdBytes	empty = {STD_DANGLING, 0};
	ChunksTrailerStored stored = chunks_trailer_Stored_of_trailer(empty, self->locators, self->stamps, self->reads);

	return chunks_trailer_Stored_stamps(&stored, result, error);
}

/* trees_node_Node_counted returns the entry count of the subtrees before the child at the position. */
static uint64
trees_node_Node_counted(const TreesNodeNode *self, size_t i)
{
	size_t		from = STD_MUL(usize, 8, i, 449, 40);
	size_t		to = STD_MUL(usize, 8, i, 449, 47);

	to = STD_ADD(usize, to, 8, 449, 47);
	std_range_check(from, to, self->prefix.len, RUST_FILE, 449, 39);
	if (std_wrapping_sub_usize(to, from) != 8)
		STD_RESULT_EXPECT_FAILED(449, 69, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	return std_u64_from_le_bytes(STD_PTR_AT(self->prefix.ptr, from, 1));
}

/* trees_node_Node_tabled returns the body offset of the address table's entry at the position. */
static uint32
trees_node_Node_tabled(const TreesNodeNode *self, size_t i)
{
	size_t		from = STD_MUL(usize, 4, i, 454, 39);
	size_t		to = STD_MUL(usize, 4, i, 454, 46);

	to = STD_ADD(usize, to, 4, 454, 46);
	std_range_check(from, to, self->table.len, RUST_FILE, 454, 38);
	if (std_wrapping_sub_usize(to, from) != 4)
		STD_RESULT_EXPECT_FAILED(454, 68, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	return std_u32_from_le_bytes(STD_PTR_AT(self->table.ptr, from, 1));
}

/* trees_node_Node_address returns the node's address. */
ChunksAddressAddress
trees_node_Node_address(const TreesNodeNode *self)
{
	return self->address;
}

/* trees_node_Node_bytes returns the node's hashed bytes as the node holds them. */
StdBytes
trees_node_Node_bytes(const TreesNodeNode *self)
{
	return self->bytes;
}

/* trees_node_Node_level returns the node's level, 0 for a leaf. */
uint8
trees_node_Node_level(const TreesNodeNode *self)
{
	return self->level;
}

/* trees_node_Node_is_leaf reports whether the node is a leaf. */
bool
trees_node_Node_is_leaf(const TreesNodeNode *self)
{
	return self->level == 0;
}

/* trees_node_Node_len returns the number of entries. */
size_t
trees_node_Node_len(const TreesNodeNode *self)
{
	return self->count;
}

/* trees_node_Node_is_empty reports whether the node has no entries. */
bool
trees_node_Node_is_empty(const TreesNodeNode *self)
{
	return self->count == 0;
}

/* trees_node_Node_key returns the key at the position. */
StdBytes
trees_node_Node_key(const TreesNodeNode *self, size_t i)
{
	StdBytes	result;
	size_t		keys_at = self->keys_at;
	size_t		start;
	size_t		end;

	if (self->key_width != 0)
	{
		size_t		at = STD_ADD(usize, keys_at, STD_MUL(usize, i, self->key_width, 490, 37),
								 490, 22);
		size_t		to = STD_ADD(usize, at, self->key_width, 491, 36);

		std_range_check(at, to, self->bytes.len, RUST_FILE, 491, 31);
		result.ptr = STD_PTR_AT(self->bytes.ptr, at, 1);
		result.len = std_wrapping_sub_usize(to, at);
		return result;
	}
	start = trees_node_u32_at(self->bytes.ptr, self->bytes.len,
							  STD_ADD(usize, keys_at, STD_MUL(usize, 4, i, 493, 55), 493, 40));
	{
		size_t		four = STD_MUL(usize, 4, i, 494, 53);
		size_t		at = STD_ADD(usize, keys_at, four, 494, 38);

		end = trees_node_u32_at(self->bytes.ptr, self->bytes.len, STD_ADD(usize, at, 4, 494, 38));
	}
	{
		size_t		from = STD_ADD(usize, self->key_data_at, start, 495, 21);
		size_t		to = STD_ADD(usize, self->key_data_at, end, 495, 47);

		std_range_check(from, to, self->bytes.len, RUST_FILE, 495, 20);
		result.ptr = STD_PTR_AT(self->bytes.ptr, from, 1);
		result.len = std_wrapping_sub_usize(to, from);
	}
	return result;
}

/* trees_node_Node_value returns a leaf's value at the position. */
StdBytes
trees_node_Node_value(const TreesNodeNode *self, size_t i)
{
	StdBytes	result;
	size_t		values_at = self->values_at;
	size_t		start;
	size_t		end;

	start = trees_node_u16_at(self->bytes.ptr, self->bytes.len,
							  STD_ADD(usize, values_at, STD_MUL(usize, 2, i, 500, 57),
									  500, 40));
	{
		size_t		twice = STD_MUL(usize, 2, i, 501, 55);
		size_t		at = STD_ADD(usize, values_at, twice, 501, 38);

		end = trees_node_u16_at(self->bytes.ptr, self->bytes.len, STD_ADD(usize, at, 2, 501, 38));
	}
	{
		size_t		from = STD_ADD(usize, self->value_data_at, start, 502, 21);
		size_t		to = STD_ADD(usize, self->value_data_at, end, 502, 49);

		std_range_check(from, to, self->bytes.len, RUST_FILE, 502, 20);
		result.ptr = STD_PTR_AT(self->bytes.ptr, from, 1);
		result.len = std_wrapping_sub_usize(to, from);
	}
	return result;
}

/* trees_node_Node_child returns an internal node's child address at the position. */
ChunksAddressAddress
trees_node_Node_child(const TreesNodeNode *self, size_t i)
{
	ChunksAddressAddress result;
	size_t		values_at = self->values_at;
	size_t		at = STD_ADD(usize, values_at, STD_MUL(usize, 32, i, 507, 35), 507, 18);
	size_t		to = STD_ADD(usize, at, 32, 508, 32);

	std_range_check(at, to, self->bytes.len, RUST_FILE, 508, 27);
	if (std_wrapping_sub_usize(to, at) != CHUNKS_ADDRESS_ADDRESS_LEN)
		STD_RESULT_EXPECT_FAILED(508, 52, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	memcpy(result.f0, STD_PTR_AT(self->bytes.ptr, at, 1), CHUNKS_ADDRESS_ADDRESS_LEN);
	return result;
}

/*
 * trees_node_Node_locator returns where the chunk the address table's entry
 * at the position names was stored, unknown when the trailer has no locator
 * for it.
 */
ChunksTrailerLocator
trees_node_Node_locator(const TreesNodeNode *self, size_t i)
{
	ChunksTrailerLocator result;
	size_t		from = STD_MUL(usize, i, CHUNKS_TRAILER_LOCATOR_LEN, 514, 27);
	size_t		to = STD_MUL(usize, STD_ADD(usize, i, 1, 514, 44), CHUNKS_TRAILER_LOCATOR_LEN,
							 514, 44);

	if (from <= to && to <= self->locators.len)
		result = chunks_trailer_Locator_read(STD_PTR_AT(self->locators.ptr, from, 1), std_wrapping_sub_usize(to, from));
	else
		result = chunks_trailer_Locator_Default_default();
	return result;
}

/* trees_node_Node_child_tree returns an internal node's child at the position as a tree. */
TreesStoreTree
trees_node_Node_child_tree(const TreesNodeNode *self, size_t i)
{
	ChunksAddressAddress child = trees_node_Node_child(self, i);

	return trees_store_Tree_at(child, trees_node_Node_locator(self, i));
}

/*
 * trees_node_Node_value_addresses_frame_0_t is value_addresses' state across
 * its pad: its arguments, its result, and the addresses it collects.
 */
typedef struct trees_node_Node_value_addresses_frame_0_t
{
	const TreesNodeNode *self;
	size_t		i;
	StdVec		addresses;
	bool		addresses_live;
	StdVec	   *result;
} trees_node_Node_value_addresses_frame_0_t;

/* trees_node_Node_value_addresses_body_0 is value_addresses' body. */
static void
trees_node_Node_value_addresses_body_0(void *frame_)
{
	trees_node_Node_value_addresses_frame_0_t *frame = frame_;

	std_vec_new(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	frame->addresses_live = true;
	trees_node_Node_value_addresses_into(frame->self, frame->i, &frame->addresses);
	frame->addresses_live = false;
	*frame->result = frame->addresses;
}

/* trees_node_Node_value_addresses_pad_0 is value_addresses' pad: the addresses, while it holds them. */
static void
trees_node_Node_value_addresses_pad_0(void *frame_)
{
	trees_node_Node_value_addresses_frame_0_t *frame = frame_;

	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
}

/*
 * trees_node_Node_value_addresses returns the addresses a leaf's value at
 * the position holds, each as its offset in the value and where its chunk
 * was stored.
 */
void
trees_node_Node_value_addresses(const TreesNodeNode *self, size_t i, StdVec *result)
{
	trees_node_Node_value_addresses_frame_0_t frame = {.self = self, .i = i, .result = result};

	std_landing(&frame, trees_node_Node_value_addresses_body_0, trees_node_Node_value_addresses_pad_0);
}

/* TreesNodeNodeValueAddressesIntoClosure0 is value_addresses_into's first partition's captures: the node and the value's start. */
typedef struct TreesNodeNodeValueAddressesIntoClosure0
{
	const TreesNodeNode *self;
	const uint32 *start;
} TreesNodeNodeValueAddressesIntoClosure0;

/* trees_node_Node_value_addresses_into_closure_0 reports whether the address table's entry lies before the value. */
static bool
trees_node_Node_value_addresses_into_closure_0(void *captures, size_t t)
{
	TreesNodeNodeValueAddressesIntoClosure0 *c = captures;

	return trees_node_Node_tabled(c->self, t) < *c->start;
}

/* TreesNodeNodeValueAddressesIntoClosure1 is value_addresses_into's second partition's captures: the node and the value's end. */
typedef struct TreesNodeNodeValueAddressesIntoClosure1
{
	const TreesNodeNode *self;
	const uint32 *end;
} TreesNodeNodeValueAddressesIntoClosure1;

/* trees_node_Node_value_addresses_into_closure_1 reports whether the address table's entry lies before the value's end. */
static bool
trees_node_Node_value_addresses_into_closure_1(void *captures, size_t t)
{
	TreesNodeNodeValueAddressesIntoClosure1 *c = captures;

	return trees_node_Node_tabled(c->self, t) < *c->end;
}

static size_t trees_node_partition(size_t n, bool (*holds) (void *, size_t), void *holds_captures,
								   void (*holds_drop) (void *));

/*
 * trees_node_Node_value_addresses_into writes into addresses, cleared first,
 * the addresses a leaf's value at the position holds, each as its offset in
 * the value and where its chunk was stored.
 */
void
trees_node_Node_value_addresses_into(const TreesNodeNode *self, size_t i, StdVec *addresses)
{
	uint32		start;
	uint32		end;
	size_t		entries;
	size_t		first;
	size_t		last;

	std_vec_clear(addresses, sizeof(chunks_TupleU32Locator_t), NULL);
	if (self->table.len == 0)
		return;
	{
		size_t		before = STD_SUB(usize, self->value_data_at, self->body, 537, 22);
		size_t		values_at = self->values_at;
		size_t		twice = STD_MUL(usize, 2, i, 537, 91);
		size_t		offset = trees_node_u16_at(self->bytes.ptr, self->bytes.len,
											   STD_ADD(usize, values_at, twice, 537, 74));

		start = (uint32) STD_ADD(usize, before, offset, 537, 21);
	}
	{
		size_t		before = STD_SUB(usize, self->value_data_at, self->body, 538, 20);
		size_t		values_at = self->values_at;
		size_t		twice = STD_MUL(usize, 2, i, 538, 89);
		size_t		at = STD_ADD(usize, values_at, twice, 538, 72);
		size_t		offset = trees_node_u16_at(self->bytes.ptr, self->bytes.len,
											   STD_ADD(usize, at, 2, 538, 72));

		end = (uint32) STD_ADD(usize, before, offset, 538, 19);
	}
	entries = STD_DIV_FOLDED(usize, self->table.len, 4);
	{
		TreesNodeNodeValueAddressesIntoClosure0 captures = {self, &start};

		first = trees_node_partition(entries, trees_node_Node_value_addresses_into_closure_0, &captures, NULL);
	}
	{
		TreesNodeNodeValueAddressesIntoClosure1 captures = {self, &end};

		last = trees_node_partition(entries, trees_node_Node_value_addresses_into_closure_1, &captures, NULL);
	}
	{
		size_t		len = first < last ? std_wrapping_sub_usize(last, first) : 0;
		StdSizeHint hint = {len, true, len};

		std_vec_extend_trusted_reserve(addresses, hint, sizeof(chunks_TupleU32Locator_t),
									   _Alignof(chunks_TupleU32Locator_t));
		for (size_t t = first, t_end = last; t < t_end; t = std_forward_unchecked_usize_Checked(t, 1))
		{
			chunks_TupleU32Locator_t item;

			item.f0 = STD_SUB(u32, trees_node_Node_tabled(self, t), start, 542, 49);
			item.f1 = trees_node_Node_locator(self, t);
			STD_VEC_AT(addresses, chunks_TupleU32Locator_t, addresses->len) = item;
			addresses->len = std_wrapping_add_usize(addresses->len, 1);
		}
	}
}

/*
 * trees_node_Node_summary returns an internal node's summary of the child's
 * subtree at the position, or none, a NULL pointer, for a node without
 * summaries.
 */
StdBytes
trees_node_Node_summary(const TreesNodeNode *self, size_t i)
{
	StdBytes	result = {NULL, 0};
	size_t		summaries_at = self->summaries_at;
	size_t		data_at;
	size_t		start;
	size_t		end;

	if (summaries_at == 0)
		return result;
	{
		size_t		n = STD_ADD(usize, self->count, 1, 550, 47);
		size_t		twice = STD_MUL(usize, 2, n, 550, 43);

		data_at = STD_ADD(usize, summaries_at, twice, 550, 23);
	}
	start = trees_node_u16_at(self->bytes.ptr, self->bytes.len,
							  STD_ADD(usize, summaries_at, STD_MUL(usize, 2, i, 551, 60),
									  551, 40));
	{
		size_t		twice = STD_MUL(usize, 2, i, 552, 58);
		size_t		at = STD_ADD(usize, summaries_at, twice, 552, 38);

		end = trees_node_u16_at(self->bytes.ptr, self->bytes.len,
								STD_ADD(usize, at, 2, 552, 38));
	}
	{
		size_t		from = STD_ADD(usize, data_at, start, 553, 26);
		size_t		to = STD_ADD(usize, data_at, end, 553, 43);

		std_range_check(from, to, self->bytes.len, RUST_FILE, 553, 25);
		result.ptr = STD_PTR_AT(self->bytes.ptr, from, 1);
		result.len = std_wrapping_sub_usize(to, from);
	}
	return result;
}

/* trees_node_Node_subtree returns the entry count of an internal node's child's subtree at the position. */
uint64
trees_node_Node_subtree(const TreesNodeNode *self, size_t i)
{
	uint64		after = trees_node_Node_counted(self, STD_ADD(usize, i, 1, 558, 22));
	uint64		before = trees_node_Node_counted(self, i);

	return STD_SUB(u64, after, before, 558, 9);
}

/* trees_node_Node_before returns the entry count of the subtrees before an internal node's child at the position. */
uint64
trees_node_Node_before(const TreesNodeNode *self, size_t i)
{
	return trees_node_Node_counted(self, i);
}

/* trees_node_Node_total returns the entry count of the node's subtree, or a blob leaf's byte count. */
uint64
trees_node_Node_total(const TreesNodeNode *self)
{
	if (self->blob)
		return (uint64) STD_SUB(usize, self->bytes.len, self->body, 569, 31);
	if (trees_node_Node_is_leaf(self))
		return (uint64) self->count;
	return trees_node_Node_counted(self, self->count);
}

/* trees_node_Node_blob_data returns a blob leaf's raw bytes, or none, a NULL pointer, for any other node. */
StdBytes
trees_node_Node_blob_data(const TreesNodeNode *self)
{
	StdBytes	result = {NULL, 0};

	if (self->blob)
	{
		std_range_from_check(self->body, self->bytes.len, RUST_FILE, 577, 38);
		result.ptr = STD_PTR_AT(self->bytes.ptr, self->body, 1);
		result.len = std_wrapping_sub_usize(self->bytes.len, self->body);
	}
	return result;
}

/*
 * trees_node_Node_partition returns the first position from from whose key
 * is not before the key, or with past not at or before it, in the order the
 * tree's keys follow.
 */
static bool
trees_node_Node_partition(const TreesNodeNode *self, HostIoIo *io, const uint8 *key, size_t key_len,
						  const TreesKeyOrder *order, size_t from, bool past, size_t *result, ChunksErrorError *error)
{
	size_t		lo;
	size_t		hi;

	if (self->comparator == trees_KeyOrder_is_bytes(order))
	{
		StdString	message;
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("tree node "), STD_FMT_ARG(0),
		STD_FMT_LIT(" is read in an order it was not written in")};
		StdFmtArg	args[] = {{&self->address, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&message, &arguments);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	lo = from;
	hi = self->count;
	while (lo < hi)
	{
		size_t		mid = STD_DIV_FOLDED(usize, STD_ADD(usize, lo, hi, 610, 23), 2);
		StdBytes	at = trees_node_Node_key(self, mid);
		StdOrdering ordering;

		if (!trees_KeyOrder_compare(order, io, at.ptr, at.len, key, key_len, &ordering, error))
			return false;
		if (ordering == STD_ORDERING_LESS || (past && ordering == STD_ORDERING_EQUAL))
			lo = STD_ADD(usize, mid, 1, 612, 70);
		else
			hi = mid;
	}
	*result = lo;
	return true;
}

/* trees_node_Node_search returns the position of the first key not before the key, in the order the tree's keys follow. */
bool
trees_node_Node_search(const TreesNodeNode *self, HostIoIo *io, const uint8 *key, size_t key_len,
					   const TreesKeyOrder *order, size_t *result, ChunksErrorError *error)
{
	return trees_node_Node_search_from(self, io, key, key_len, order, 0, result, error);
}

/* trees_node_Node_search_from returns the position of the first key from from not before the key. */
bool
trees_node_Node_search_from(const TreesNodeNode *self, HostIoIo *io, const uint8 *key, size_t key_len,
							const TreesKeyOrder *order, size_t from, size_t *result, ChunksErrorError *error)
{
	return trees_node_Node_partition(self, io, key, key_len, order, from, false, result, error);
}

/* trees_node_Node_search_past returns the position of the first key after the key. */
bool
trees_node_Node_search_past(const TreesNodeNode *self, HostIoIo *io, const uint8 *key, size_t key_len,
							const TreesKeyOrder *order, size_t *result, ChunksErrorError *error)
{
	return trees_node_Node_partition(self, io, key, key_len, order, 0, true, result, error);
}

/* trees_node_partition_frame_0_t is partition's state across its pad: its arguments and its result. */
typedef struct trees_node_partition_frame_0_t
{
	size_t		n;
	bool		(*holds) (void *, size_t);
	void	   *holds_captures;
	void		(*holds_drop) (void *);
	bool		holds_live;
	size_t		result;
} trees_node_partition_frame_0_t;

/* trees_node_partition_body_0 is partition's body. */
static void
trees_node_partition_body_0(void *frame_)
{
	trees_node_partition_frame_0_t *frame = frame_;
	size_t		lo = 0;
	size_t		hi = frame->n;

	while (lo < hi)
	{
		size_t		mid = STD_DIV_FOLDED(usize, STD_ADD(usize, lo, hi, 622, 19), 2);

		if (frame->holds(frame->holds_captures, mid))
			lo = STD_ADD(usize, mid, 1, 623, 30);
		else
			hi = mid;
	}
	frame->holds_live = false;
	if (frame->holds_drop != NULL)
		frame->holds_drop(frame->holds_captures);
	frame->result = lo;
}

/* trees_node_partition_pad_0 is partition's pad: the closure's drop, while it holds it. */
static void
trees_node_partition_pad_0(void *frame_)
{
	trees_node_partition_frame_0_t *frame = frame_;

	if (frame->holds_live && frame->holds_drop != NULL)
		frame->holds_drop(frame->holds_captures);
}

/* trees_node_partition returns the first of the positions below n that holds is false for, holds true for every one before it. */
static size_t
trees_node_partition(size_t n, bool (*holds) (void *, size_t), void *holds_captures, void (*holds_drop) (void *))
{
	trees_node_partition_frame_0_t frame = {.n = n, .holds = holds, .holds_captures = holds_captures,
	.holds_drop = holds_drop, .holds_live = true};

	std_landing(&frame, trees_node_partition_body_0, trees_node_partition_pad_0);
	return frame.result;
}

/* trees_node_key_width returns the length every key has when it is the same nonzero length below 256, or 0. */
static uint8
trees_node_key_width(const StdBytes *keys, size_t keys_len)
{
	size_t		width = keys_len > 0 ? keys[0].len : 0;

	if (width > 0 && width < 256)
	{
		bool		all = true;

		for (size_t k = 0, k_len = keys_len; k < k_len; k++)
		{
			if (!(keys[k].len == width))
			{
				all = false;
				break;
			}
		}
		if (all)
			return (uint8) width;
	}
	return 0;
}

/* trees_node_offsets appends an offset per item and the end, then the items. */
static void
trees_node_offsets(StdVec *out, const StdBytes *items, size_t items_len)
{
	size_t		at = 0;
	uint8		le[2];

	for (size_t k = 0, k_len = items_len; k < k_len; k++)
	{
		std_u16_to_le_bytes((uint16) at, le);
		std_vec_extend_from_slice_Checked(out, le, 2, 1, 1);
		at = STD_ADD(usize, at, items[k].len, 668, 9);
	}
	if (!(at <= (size_t) PG_UINT16_MAX))
		STD_PANIC(670, 5, "tree node data is larger than 64 KB");
	std_u16_to_le_bytes((uint16) at, le);
	std_vec_extend_from_slice_Checked(out, le, 2, 1, 1);
	for (size_t k = 0, k_len = items_len; k < k_len; k++)
		std_vec_extend_from_slice_Checked(out, items[k].ptr, items[k].len, 1, 1);
}

/* trees_node_key_layout appends the keys, fixed-width when every key has the same nonzero length, and returns the width. */
static uint8
trees_node_key_layout(StdVec *out, const StdBytes *keys, size_t keys_len)
{
	uint8		width = trees_node_key_width(keys, keys_len);

	std_vec_u8_push_Checked(out, width);
	if (width > 0)
	{
		for (size_t k = 0, k_len = keys_len; k < k_len; k++)
			std_vec_extend_from_slice_Checked(out, keys[k].ptr, keys[k].len, 1, 1);
		return width;
	}
	trees_node_offsets(out, keys, keys_len);
	return 0;
}

/*
 * trees_node_front_code appends a leaf's key width and its keys, each as the
 * length of the prefix it shares with the key before it, its remaining
 * length when the keys vary in length, and its remaining bytes.
 */
static void
trees_node_front_code(StdVec *out, const StdBytes *keys, size_t keys_len)
{
	uint8		width = trees_node_key_width(keys, keys_len);
	StdBytes	before = {STD_DANGLING, 0};

	std_vec_u8_push_Checked(out, width);
	for (size_t k = 0, k_len = keys_len; k < k_len; k++)
	{
		const StdBytes *key = &keys[k];
		size_t		shared = trees_node_shared_prefix(before.ptr, before.len, key->ptr, key->len);

		chunks_varint_put_varint(out, (uint64) shared);
		if (width == 0)
			chunks_varint_put_varint(out, (uint64) STD_SUB(usize, key->len, shared, 656, 29));
		std_range_from_check(shared, key->len, RUST_FILE, 658, 35);
		std_vec_extend_from_slice_Checked(out, STD_PTR_AT(key->ptr, shared, 1), std_wrapping_sub_usize(key->len, shared), 1, 1);
		before = *key;
	}
}

/*
 * trees_node_encode_leaf_frame_0_t is encode_leaf's state across its pad:
 * its arguments, its result, and the body and address table it builds.
 */
typedef struct trees_node_encode_leaf_frame_0_t
{
	const StdBytes *keys;
	size_t		keys_len;
	const StdBytes *values;
	size_t		values_len;
	const StdVec *addresses;
	size_t		addresses_len;
	bool		comparator;
	StdVec		body;
	bool		body_live;
	StdVec		table;
	bool		table_live;
	StdVec	   *result;
} trees_node_encode_leaf_frame_0_t;

/* trees_node_encode_leaf_body_0 is encode_leaf's body. */
static void
trees_node_encode_leaf_body_0(void *frame_)
{
	trees_node_encode_leaf_frame_0_t *frame = frame_;
	size_t		value_data_at;
	size_t		at;
	ChunksHeaderHeader header;

	std_vec_new(&frame->body, 1, 1);
	frame->body_live = true;
	chunks_varint_put_varint(&frame->body, (uint64) frame->keys_len);
	trees_node_front_code(&frame->body, frame->keys, frame->keys_len);
	{
		size_t		len = frame->body.len;
		size_t		n = STD_ADD(usize, frame->values_len, 1, 681, 42);
		size_t		twice = STD_MUL(usize, 2, n, 681, 38);

		value_data_at = STD_ADD(usize, len, twice, 681, 25);
	}
	trees_node_offsets(&frame->body, frame->values, frame->values_len);
	std_vec_new(&frame->table, sizeof(uint32), _Alignof(uint32));
	frame->table_live = true;
	at = value_data_at;
	for (size_t k = 0, k_len = std_min_usize(frame->values_len, frame->addresses_len); k < k_len; k++)
	{
		const StdBytes *value = &frame->values[k];
		const StdVec *inner = &frame->addresses[k];
		StdSizeHint hint = {inner->len, true, inner->len};

		std_vec_extend_trusted_reserve(&frame->table, hint, sizeof(uint32), _Alignof(uint32));
		for (size_t j = 0, j_len = inner->len; j < j_len; j++)
		{
			size_t		o = (size_t) STD_VEC_AT(inner, uint32, j);

			STD_VEC_AT(&frame->table, uint32, frame->table.len) = (uint32) STD_ADD(usize, at, o,
																				   686, 44);
			frame->table.len = std_wrapping_add_usize(frame->table.len, 1);
		}
		at = STD_ADD(usize, at, value->len, 687, 9);
	}
	header.kind = CHUNKS_HEADER_TREE_LEAF;
	header.flags = frame->comparator ? TREES_NODE_COMPARATOR : 0;
	header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
	header.addresses.u.offsets = frame->table;
	chunks_header_Header_write(&header, frame->body.ptr, frame->body.len, frame->result);
	frame->table_live = false;
	chunks_header_Header_drop(&header);
	frame->body_live = false;
	std_vec_free_Checked(&frame->body, 1, 1);
}

/* trees_node_encode_leaf_pad_0 is encode_leaf's pad: the address table and the body, while it holds them. */
static void
trees_node_encode_leaf_pad_0(void *frame_)
{
	trees_node_encode_leaf_frame_0_t *frame = frame_;

	if (frame->table_live)
		std_vec_free_Checked(&frame->table, sizeof(uint32), _Alignof(uint32));
	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
}

/*
 * trees_node_encode_leaf returns a leaf's hashed bytes, given in addresses
 * the offsets of the addresses each value holds (out-of-line parts), each a
 * Vec<u32>.
 */
void
trees_node_encode_leaf(const StdBytes *keys, size_t keys_len, const StdBytes *values, size_t values_len,
					   const StdVec *addresses, size_t addresses_len, bool comparator, StdVec *result)
{
	trees_node_encode_leaf_frame_0_t frame = {.keys = keys, .keys_len = keys_len, .values = values,
		.values_len = values_len, .addresses = addresses, .addresses_len = addresses_len,
	.comparator = comparator, .result = result};

	std_landing(&frame, trees_node_encode_leaf_body_0, trees_node_encode_leaf_pad_0);
}

/* trees_node_encode_internal returns an internal node's hashed bytes. */
void
trees_node_encode_internal(uint8 level, const StdBytes *keys, size_t keys_len, const ChunksAddressAddress *children,
						   size_t children_len, const uint64 *counts, size_t counts_len, StdVec *result)
{
	trees_node_encode_internal_with_summaries(level, keys, keys_len, children, children_len, counts, counts_len,
											  NULL, 0, false, result);
}

/*
 * trees_node_encode_internal_with_summaries_frame_0_t is
 * encode_internal_with_summaries' state across its pad: its arguments, its
 * result, and the body it builds.
 */
typedef struct trees_node_encode_internal_with_summaries_frame_0_t
{
	uint8		level;
	const StdBytes *keys;
	size_t		keys_len;
	const ChunksAddressAddress *children;
	size_t		children_len;
	const uint64 *counts;
	size_t		counts_len;
	const StdBytes *summaries;
	size_t		summaries_len;
	bool		comparator;
	StdVec		body;
	bool		body_live;
	StdVec	   *result;
} trees_node_encode_internal_with_summaries_frame_0_t;

/* trees_node_encode_internal_with_summaries_body_0 is encode_internal_with_summaries' body. */
static void
trees_node_encode_internal_with_summaries_body_0(void *frame_)
{
	trees_node_encode_internal_with_summaries_frame_0_t *frame = frame_;
	uint32		first;
	ChunksHeaderHeader header;

	{
		uint8	   *boxed = std_box_new_uninit_layout_Checked(1, 1);

		STD_CHECK_ALIGNED(boxed, 1, 707, 20);
		STD_CHECK_NULL_DEREF(boxed, 707, 20);
		boxed[0] = frame->level;
		std_box_slice_into_vec_Checked(&frame->body, boxed, 1, 1);
	}
	frame->body_live = true;
	chunks_varint_put_varint(&frame->body, (uint64) frame->keys_len);
	(void) trees_node_key_layout(&frame->body, frame->keys, frame->keys_len);
	first = (uint32) frame->body.len;
	for (size_t k = 0, k_len = frame->children_len; k < k_len; k++)
		std_vec_extend_from_slice_Checked(&frame->body, frame->children[k].f0, CHUNKS_ADDRESS_ADDRESS_LEN, 1, 1);
	for (size_t k = 0, k_len = frame->counts_len; k < k_len; k++)
		chunks_varint_put_varint(&frame->body, frame->counts[k]);
	if (frame->summaries != NULL)
	{
		const StdBytes *summaries_2 = frame->summaries;

		trees_node_offsets(&frame->body, summaries_2, frame->summaries_len);
	}
	header.kind = CHUNKS_HEADER_TREE_INTERNAL;
	header.flags = (frame->summaries != NULL ? TREES_NODE_SUMMARIES : 0) |
		(frame->comparator ? TREES_NODE_COMPARATOR : 0);
	header.addresses.tag = CHUNKS_HEADER_ADDRESSES_STRIDE;
	header.addresses.u.stride.first = first;
	header.addresses.u.stride.stride = 32;
	header.addresses.u.stride.count = (uint32) frame->children_len;
	chunks_header_Header_write(&header, frame->body.ptr, frame->body.len, frame->result);
	frame->body_live = false;
	std_vec_free_Checked(&frame->body, 1, 1);
}

/* trees_node_encode_internal_with_summaries_pad_0 is encode_internal_with_summaries' pad: the body, while it holds it. */
static void
trees_node_encode_internal_with_summaries_pad_0(void *frame_)
{
	trees_node_encode_internal_with_summaries_frame_0_t *frame = frame_;

	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
}

/*
 * trees_node_encode_internal_with_summaries returns an internal node's
 * hashed bytes, with each child's summary when summaries is not NULL.
 */
void
trees_node_encode_internal_with_summaries(uint8 level, const StdBytes *keys, size_t keys_len,
										  const ChunksAddressAddress *children, size_t children_len,
										  const uint64 *counts, size_t counts_len, const StdBytes *summaries,
										  size_t summaries_len, bool comparator, StdVec *result)
{
	trees_node_encode_internal_with_summaries_frame_0_t frame = {.level = level, .keys = keys,
		.keys_len = keys_len, .children = children, .children_len = children_len, .counts = counts,
		.counts_len = counts_len, .summaries = summaries, .summaries_len = summaries_len,
	.comparator = comparator, .result = result};

	std_landing(&frame, trees_node_encode_internal_with_summaries_body_0,
				trees_node_encode_internal_with_summaries_pad_0);
}

/* trees_node_TupleU32Locator_Debug_fmt is (u32, Locator)'s Debug. */
bool
trees_node_TupleU32Locator_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleU32Locator_t *self = value;
	StdTupleRef tuple = {2, {{&self->f0, std_u32_Debug_fmt}, {&self->f1, chunks_trailer_Locator_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* trees_node_SliceTupleU32Locator_Debug_fmt is [(u32, Locator)]'s Debug: a list of the pairs. */
bool
trees_node_SliceTupleU32Locator_Debug_fmt(const chunks_TupleU32Locator_t *self, size_t self_len,
										  StdFormatter *f)
{
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = self_len; i < i_len; i++)
		std_DebugList_entry(&list, &self[i], trees_node_TupleU32Locator_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* trees_node_VecTupleU32Locator_Debug_fmt is Vec<(u32, Locator)>'s Debug, its slice's. */
bool
trees_node_VecTupleU32Locator_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;

	return trees_node_SliceTupleU32Locator_Debug_fmt(self->ptr, self->len, f);
}

/*
 * trees_node_SliceTupleU32Locator_PartialEqSliceTupleU32Locator_eq is
 * [(u32, Locator)]'s PartialEq: the same length, and each pair's offsets and
 * locators equal.
 */
bool
trees_node_SliceTupleU32Locator_PartialEqSliceTupleU32Locator_eq(const chunks_TupleU32Locator_t *self,
																 size_t self_len,
																 const chunks_TupleU32Locator_t *other,
																 size_t other_len)
{
	if (self_len != other_len)
		return false;
	for (size_t i = 0, i_len = self_len; i < i_len; i++)
		if (!(self[i].f0 == other[i].f0 && chunks_trailer_Locator_PartialEq_eq(&self[i].f1, &other[i].f1)))
			return false;
	return true;
}

/* trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq is Vec<(u32, Locator)>'s PartialEq, its slices'. */
bool
trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq(const StdVec *self, const StdVec *other)
{
	return trees_node_SliceTupleU32Locator_PartialEqSliceTupleU32Locator_eq(self->ptr, self->len, other->ptr,
																			other->len);
}

#ifdef DOLTGRES_TEST_TREES

/* trees_node_tests_VecU8_drop is a Vec<u8>'s drop glue, an element of a Vec<Vec<u8>>. */
static void
trees_node_tests_VecU8_drop(void *elem)
{
	std_vec_free_Checked(elem, 1, 1);
}

/* trees_node_tests_VecU32_drop is a Vec<u32>'s drop glue, an element of a Vec<Vec<u32>>. */
static void
trees_node_tests_VecU32_drop(void *elem)
{
	std_vec_free_Checked(elem, sizeof(uint32), _Alignof(uint32));
}

/* trees_node_tests_SliceAddress_Debug_fmt is [Address]'s Debug: a list of the addresses. */
static bool
trees_node_tests_SliceAddress_Debug_fmt(const ChunksAddressAddress *self, size_t self_len, StdFormatter *f)
{
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = self_len; i < i_len; i++)
		std_DebugList_entry(&list, &self[i], chunks_address_Address_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* trees_node_tests_VecAddress_Debug_fmt is Vec<Address>'s Debug, its slice's. */
static bool
trees_node_tests_VecAddress_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;

	return trees_node_tests_SliceAddress_Debug_fmt(self->ptr, self->len, f);
}

/* trees_node_tests_SliceAddress_PartialEqSliceAddress_eq is [Address]'s PartialEq: the same length, and each pair of addresses equal. */
static bool
trees_node_tests_SliceAddress_PartialEqSliceAddress_eq(const ChunksAddressAddress *self, size_t self_len,
													   const ChunksAddressAddress *other, size_t other_len)
{
	if (self_len != other_len)
		return false;
	for (size_t i = 0, i_len = self_len; i < i_len; i++)
		if (!chunks_address_Address_PartialEq_eq(&self[i], &other[i]))
			return false;
	return true;
}

/* trees_node_tests_VecAddress_PartialEqVecAddress_eq is Vec<Address>'s PartialEq, its slices'. */
static bool
trees_node_tests_VecAddress_PartialEqVecAddress_eq(const StdVec *self, const StdVec *other)
{
	return trees_node_tests_SliceAddress_PartialEqSliceAddress_eq(self->ptr, self->len, other->ptr, other->len);
}

/*
 * test_trees_node_tests_leaves_and_internal_nodes_round_trip_frame_0_t is the
 * test's state across its pad: every value it holds, while live, and an
 * unwrap's error.
 */
typedef struct test_trees_node_tests_leaves_and_internal_nodes_round_trip_frame_0_t
{
	StdVec		keys;
	bool		keys_live;
	StdVec		with_address;
	bool		with_address_live;
	StdVec		values;
	bool		values_live;
	StdVec		offsets[3];
	bool		offsets_live[3];
	StdVec		hashed;
	bool		hashed_live;
	StdVec		flat;
	bool		flat_live;
	StdVec		left;
	bool		left_live;
	StdVec		right;
	bool		right_live;
	StdVec		addresses;
	bool		addresses_live;
	StdVec		addresses_2;
	bool		addresses_2_live;
	SimcTestIo	test_io;
	bool		test_io_live;
	StdVec		fixed;
	bool		fixed_live;
	StdVec		hashed_2;
	bool		hashed_2_live;
	StdVec		flat_2;
	bool		flat_2_live;
	StdVec		empty;
	bool		empty_live;
	StdVec		flat_3;
	bool		flat_3_live;
	ChunksErrorError error;
	bool		error_live;
} test_trees_node_tests_leaves_and_internal_nodes_round_trip_frame_0_t;

/*
 * test_trees_node_tests_leaves_and_internal_nodes_round_trip_body_0 is the
 * test: a leaf and an internal node round-trip their keys, values, address
 * tables, children, and counts; a leaf's search finds each key's position; an
 * empty leaf decodes empty, and a truncated node is refused.
 */
static void
test_trees_node_tests_leaves_and_internal_nodes_round_trip_body_0(void *frame_)
{
	test_trees_node_tests_leaves_and_internal_nodes_round_trip_frame_0_t *frame = frame_;
	static const TreesKeyOrder bytes_order = {.tag = TREES_KEY_ORDER_BYTES};
	static const uint8 fixed_0[] = {0, 1};
	static const uint8 fixed_1[] = {0, 2};
	static const uint64 counts[] = {300, 5};
	const uint8 *none = STD_DANGLING_FOR(1);
	ChunksAddressAddress blob;
	TreesNodeNode node;
	TreesNodeNode node_2;
	HostIoIo   *io;
	ChunksAddressAddress children[2];

	{
		StdBytes	items[3] = {{(const uint8 *) "apple", 5}, {(const uint8 *) "banana", 6},
		{(const uint8 *) "cherry", 6}};
		StdBytes   *boxed = std_box_new_uninit_layout_Checked(sizeof(items), _Alignof(StdBytes));

		STD_CHECK_ALIGNED(boxed, _Alignof(StdBytes), 727, 32);
		STD_CHECK_NULL_DEREF(boxed, 727, 32);
		memcpy(boxed, items, sizeof(items));
		std_box_slice_into_vec_Checked(&frame->keys, boxed, 3, sizeof(StdBytes));
		frame->keys_live = true;
	}
	blob = chunks_address_Address_of((const uint8 *) "blob", 4);
	std_slice_to_vec_Checked(&frame->with_address, "xx", 2, 1, 1);
	frame->with_address_live = true;
	std_vec_extend_from_slice_Checked(&frame->with_address, blob.f0, CHUNKS_ADDRESS_ADDRESS_LEN, 1, 1);
	{
		StdBytes	items[3] = {{(const uint8 *) "1", 1}, {frame->with_address.ptr, frame->with_address.len},
		{(const uint8 *) "", 0}};
		StdBytes   *boxed = std_box_new_uninit_layout_Checked(sizeof(items), _Alignof(StdBytes));

		STD_CHECK_ALIGNED(boxed, _Alignof(StdBytes), 731, 34);
		STD_CHECK_NULL_DEREF(boxed, 731, 34);
		memcpy(boxed, items, sizeof(items));
		std_box_slice_into_vec_Checked(&frame->values, boxed, 3, sizeof(StdBytes));
		frame->values_live = true;
	}
	std_vec_new(&frame->offsets[0], sizeof(uint32), _Alignof(uint32));
	frame->offsets_live[0] = true;
	{
		uint32	   *boxed = std_box_new_uninit_layout_Checked(sizeof(uint32), _Alignof(uint32));

		STD_CHECK_ALIGNED(boxed, _Alignof(uint32), 732, 60);
		STD_CHECK_NULL_DEREF(boxed, 732, 60);
		boxed[0] = 2;
		std_box_slice_into_vec_Checked(&frame->offsets[1], boxed, 1, sizeof(uint32));
		frame->offsets_live[1] = true;
	}
	std_vec_new(&frame->offsets[2], sizeof(uint32), _Alignof(uint32));
	frame->offsets_live[2] = true;
	trees_node_encode_leaf(frame->keys.ptr, frame->keys.len, frame->values.ptr, frame->values.len, frame->offsets, 3,
						   false, &frame->hashed);
	frame->hashed_live = true;
	for (size_t k = 0, k_len = 3; k < k_len; k++)
	{
		frame->offsets_live[k] = false;
		std_vec_free_Checked(&frame->offsets[k], sizeof(uint32), _Alignof(uint32));
	}
	if (!trees_node_flat_of(chunks_address_Address_of(frame->hashed.ptr, frame->hashed.len), frame->hashed.ptr,
							frame->hashed.len, none, 0, none, 0, 0, &frame->flat, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(733, 72, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->flat_live = true;
	node = trees_node_Node_view(frame->flat.ptr, frame->flat.len);
	STD_ASSERT(735, 9, trees_node_Node_is_leaf(&node), "node.is_leaf()");
	{
		size_t		left = trees_node_Node_len(&node);
		size_t		right = 3;

		STD_ASSERT_EQ(736, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	for (size_t i = 0, i_end = 3; i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		StdBytes	got = trees_node_Node_key(&node, i);
		StdBytes	want;

		std_index_check(i, frame->keys.len, RUST_FILE, 738, 41);
		want = STD_VEC_AT(&frame->keys, StdBytes, i);
		STD_ASSERT_EQ(738, 13, got.len == want.len && (want.len == 0 || memcmp(got.ptr, want.ptr, want.len) == 0),
					  &got, std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
		got = trees_node_Node_value(&node, i);
		std_index_check(i, frame->values.len, RUST_FILE, 739, 45);
		want = STD_VEC_AT(&frame->values, StdBytes, i);
		STD_ASSERT_EQ(739, 13, got.len == want.len && (want.len == 0 || memcmp(got.ptr, want.ptr, want.len) == 0),
					  &got, std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
	}
	if (!chunks_header_addresses(frame->hashed.ptr, frame->hashed.len, &frame->left, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(741, 55, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->left_live = true;
	{
		ChunksAddressAddress *boxed = std_box_new_uninit_layout_Checked(sizeof(ChunksAddressAddress),
																		_Alignof(ChunksAddressAddress));

		STD_CHECK_ALIGNED(boxed, _Alignof(ChunksAddressAddress), 741, 65);
		STD_CHECK_NULL_DEREF(boxed, 741, 65);
		boxed[0] = blob;
		std_box_slice_into_vec_Checked(&frame->right, boxed, 1, sizeof(ChunksAddressAddress));
		frame->right_live = true;
	}
	STD_ASSERT_EQ(741, 9, trees_node_tests_VecAddress_PartialEqVecAddress_eq(&frame->left, &frame->right),
				  &frame->left, trees_node_tests_VecAddress_Debug_fmt, &frame->right,
				  trees_node_tests_VecAddress_Debug_fmt);
	frame->right_live = false;
	std_vec_free_Checked(&frame->right, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->left_live = false;
	std_vec_free_Checked(&frame->left, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	trees_node_Node_value_addresses(&node, 1, &frame->addresses);
	frame->addresses_live = true;
	{
		chunks_TupleU32Locator_t *boxed = std_box_new_uninit_layout_Checked(sizeof(chunks_TupleU32Locator_t),
																			_Alignof(chunks_TupleU32Locator_t));

		STD_CHECK_ALIGNED(boxed, _Alignof(chunks_TupleU32Locator_t), 742, 45);
		STD_CHECK_NULL_DEREF(boxed, 742, 45);
		boxed[0].f0 = 2;
		boxed[0].f1 = chunks_trailer_Locator_Default_default();
		std_box_slice_into_vec_Checked(&frame->addresses_2, boxed, 1, sizeof(chunks_TupleU32Locator_t));
		frame->addresses_2_live = true;
	}
	STD_ASSERT_EQ(742, 9,
				  trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq(&frame->addresses,
																			   &frame->addresses_2),
				  &frame->addresses, trees_node_VecTupleU32Locator_Debug_fmt, &frame->addresses_2,
				  trees_node_VecTupleU32Locator_Debug_fmt);
	frame->addresses_2_live = false;
	std_vec_free_Checked(&frame->addresses_2, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	frame->addresses_live = false;
	std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	{
		bool		empty;

		trees_node_Node_value_addresses(&node, 0, &frame->addresses);
		frame->addresses_live = true;
		empty = frame->addresses.len == 0;
		if (empty)
		{
			trees_node_Node_value_addresses(&node, 2, &frame->addresses_2);
			frame->addresses_2_live = true;
			empty = frame->addresses_2.len == 0;
			frame->addresses_2_live = false;
			std_vec_free_Checked(&frame->addresses_2, sizeof(chunks_TupleU32Locator_t),
								 _Alignof(chunks_TupleU32Locator_t));
		}
		frame->addresses_live = false;
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		STD_ASSERT(743, 9, empty, "node.value_addresses(0).is_empty() && node.value_addresses(2).is_empty()");
	}
	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	{
		size_t		got;
		size_t		want = 1;

		if (!trees_node_Node_search(&node, io, (const uint8 *) "b", 1, &bytes_order, &got, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(746, 60, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		STD_ASSERT_EQ(746, 9, got == want, &got, std_usize_Debug_fmt, &want, std_usize_Debug_fmt);
	}
	{
		size_t		got;
		size_t		want = 1;

		if (!trees_node_Node_search(&node, io, (const uint8 *) "banana", 6, &bytes_order, &got, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(747, 65, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		STD_ASSERT_EQ(747, 9, got == want, &got, std_usize_Debug_fmt, &want, std_usize_Debug_fmt);
	}
	{
		size_t		got;
		size_t		want = 3;

		if (!trees_node_Node_search(&node, io, (const uint8 *) "z", 1, &bytes_order, &got, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(748, 60, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		STD_ASSERT_EQ(748, 9, got == want, &got, std_usize_Debug_fmt, &want, std_usize_Debug_fmt);
	}
	{
		StdBytes	items[2] = {{fixed_0, sizeof(fixed_0)}, {fixed_1, sizeof(fixed_1)}};
		StdBytes   *boxed = std_box_new_uninit_layout_Checked(sizeof(items), _Alignof(StdBytes));

		STD_CHECK_ALIGNED(boxed, _Alignof(StdBytes), 750, 33);
		STD_CHECK_NULL_DEREF(boxed, 750, 33);
		memcpy(boxed, items, sizeof(items));
		std_box_slice_into_vec_Checked(&frame->fixed, boxed, 2, sizeof(StdBytes));
		frame->fixed_live = true;
	}
	children[0] = chunks_address_Address_of((const uint8 *) "a", 1);
	children[1] = chunks_address_Address_of((const uint8 *) "b", 1);
	trees_node_encode_internal(2, frame->fixed.ptr, frame->fixed.len, children, 2, counts, 2, &frame->hashed_2);
	frame->hashed_2_live = true;
	if (!trees_node_flat_of(chunks_address_Address_of(frame->hashed_2.ptr, frame->hashed_2.len), frame->hashed_2.ptr,
							frame->hashed_2.len, none, 0, none, 0, 0, &frame->flat_2, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(753, 72, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->flat_2_live = true;
	node_2 = trees_node_Node_view(frame->flat_2.ptr, frame->flat_2.len);
	{
		uint8		level = trees_node_Node_level(&node_2);
		size_t		len = trees_node_Node_len(&node_2);
		uint64		total = trees_node_Node_total(&node_2);
		uint8		want_level = 2;
		size_t		want_len = 2;
		uint64		want_total = 305;

		STD_ASSERT_EQ(755, 9, level == want_level && len == want_len && total == want_total,
					  &STD_TUPLE(3, STD_ITEM(&level, std_u8_Debug_fmt), STD_ITEM(&len, std_usize_Debug_fmt),
								 STD_ITEM(&total, std_u64_Debug_fmt)), std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&want_level, std_u8_Debug_fmt), STD_ITEM(&want_len, std_usize_Debug_fmt),
								 STD_ITEM(&want_total, std_u64_Debug_fmt)), std_tuple_Debug_fmt);
	}
	{
		ChunksAddressAddress child = trees_node_Node_child(&node_2, 1);
		uint64		subtree = trees_node_Node_subtree(&node_2, 1);
		uint64		before = trees_node_Node_before(&node_2, 1);
		uint64		want_subtree = 5;
		uint64		want_before = 300;

		STD_ASSERT_EQ(756, 9, chunks_address_Address_PartialEq_eq(&child, &children[1]) &&
					  subtree == want_subtree && before == want_before,
					  &STD_TUPLE(3, STD_ITEM(&child, chunks_address_Address_Debug_fmt),
								 STD_ITEM(&subtree, std_u64_Debug_fmt), STD_ITEM(&before, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&children[1], chunks_address_Address_Debug_fmt),
								 STD_ITEM(&want_subtree, std_u64_Debug_fmt), STD_ITEM(&want_before, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	{
		StdBytes	got = trees_node_Node_key(&node_2, 1);
		StdBytes	want = {fixed_1, sizeof(fixed_1)};

		STD_ASSERT_EQ(757, 9, got.len == want.len && memcmp(got.ptr, want.ptr, want.len) == 0, &got,
					  std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
	}
	if (!chunks_header_addresses(frame->hashed_2.ptr, frame->hashed_2.len, &frame->left, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(758, 55, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->left_live = true;
	std_slice_to_vec_Checked(&frame->right, children, 2, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->right_live = true;
	STD_ASSERT_EQ(758, 9, trees_node_tests_VecAddress_PartialEqVecAddress_eq(&frame->left, &frame->right),
				  &frame->left, trees_node_tests_VecAddress_Debug_fmt, &frame->right,
				  trees_node_tests_VecAddress_Debug_fmt);
	frame->right_live = false;
	std_vec_free_Checked(&frame->right, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->left_live = false;
	std_vec_free_Checked(&frame->left, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	trees_node_encode_leaf(STD_DANGLING_FOR(_Alignof(StdBytes)), 0, STD_DANGLING_FOR(_Alignof(StdBytes)), 0,
						   STD_DANGLING_FOR(_Alignof(StdVec)), 0, false, &frame->empty);
	frame->empty_live = true;
	{
		bool		empty;

		if (!trees_node_flat_of(chunks_address_Address_of(frame->empty.ptr, frame->empty.len), frame->empty.ptr,
								frame->empty.len, none, 0, none, 0, 0, &frame->flat_3, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(761, 79, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		frame->flat_3_live = true;
		{
			TreesNodeNode viewed = trees_node_Node_view(frame->flat_3.ptr, frame->flat_3.len);

			empty = trees_node_Node_is_empty(&viewed);
		}
		frame->flat_3_live = false;
		std_vec_free_Checked(&frame->flat_3, 1, 1);
		STD_ASSERT(761, 9, empty,
				   "Node::view(&flat_of(Address::of(&empty), &empty, &[], &[], 0).unwrap()).is_empty()");
	}
	{
		ChunksAddressAddress address = chunks_address_Address_of(none, 0);
		size_t		end = STD_SUB(usize, frame->hashed_2.len, 1, 762, 53);
		StdVec		flat;
		ChunksErrorError error;
		bool		ok;

		std_range_to_check(end, frame->hashed_2.len, RUST_FILE, 762, 50);
		ok = trees_node_flat_of(address, frame->hashed_2.ptr, end, none, 0, none, 0, 0, &flat, &error);
		if (ok)
			std_vec_free_Checked(&flat, 1, 1);
		else
			chunks_error_Error_drop(&error);
		STD_ASSERT(762, 9, !ok, "flat_of(Address::of(b\"\"), &hashed[..hashed.len() - 1], &[], &[], 0).is_err()");
	}
	frame->empty_live = false;
	std_vec_free_Checked(&frame->empty, 1, 1);
	frame->flat_2_live = false;
	std_vec_free_Checked(&frame->flat_2, 1, 1);
	frame->hashed_2_live = false;
	std_vec_free_Checked(&frame->hashed_2, 1, 1);
	frame->fixed_live = false;
	std_vec_free_Checked(&frame->fixed, sizeof(StdBytes), _Alignof(StdBytes));
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
	frame->flat_live = false;
	std_vec_free_Checked(&frame->flat, 1, 1);
	frame->hashed_live = false;
	std_vec_free_Checked(&frame->hashed, 1, 1);
	frame->values_live = false;
	std_vec_free_Checked(&frame->values, sizeof(StdBytes), _Alignof(StdBytes));
	frame->with_address_live = false;
	std_vec_free_Checked(&frame->with_address, 1, 1);
	frame->keys_live = false;
	std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
}

/*
 * test_trees_node_tests_leaves_and_internal_nodes_round_trip_pad_0 is the
 * test's pad: an unwrap's error, then every other value its frame holds, in
 * the reverse of the order the test makes them, while live.
 */
static void
test_trees_node_tests_leaves_and_internal_nodes_round_trip_pad_0(void *frame_)
{
	test_trees_node_tests_leaves_and_internal_nodes_round_trip_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->flat_3_live)
		std_vec_free_Checked(&frame->flat_3, 1, 1);
	if (frame->empty_live)
		std_vec_free_Checked(&frame->empty, 1, 1);
	if (frame->flat_2_live)
		std_vec_free_Checked(&frame->flat_2, 1, 1);
	if (frame->hashed_2_live)
		std_vec_free_Checked(&frame->hashed_2, 1, 1);
	if (frame->fixed_live)
		std_vec_free_Checked(&frame->fixed, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
	if (frame->addresses_2_live)
		std_vec_free_Checked(&frame->addresses_2, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	if (frame->right_live)
		std_vec_free_Checked(&frame->right, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->left_live)
		std_vec_free_Checked(&frame->left, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->flat_live)
		std_vec_free_Checked(&frame->flat, 1, 1);
	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
	for (size_t k = 0, k_len = 3; k < k_len; k++)
		if (frame->offsets_live[k])
			std_vec_free_Checked(&frame->offsets[k], sizeof(uint32), _Alignof(uint32));
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->with_address_live)
		std_vec_free_Checked(&frame->with_address, 1, 1);
	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
}

/* test_trees_node_tests_leaves_and_internal_nodes_round_trip is the test leaves_and_internal_nodes_round_trip. */
void
test_trees_node_tests_leaves_and_internal_nodes_round_trip(void)
{
	test_trees_node_tests_leaves_and_internal_nodes_round_trip_frame_0_t frame = {0};

	std_landing(&frame, test_trees_node_tests_leaves_and_internal_nodes_round_trip_body_0,
				test_trees_node_tests_leaves_and_internal_nodes_round_trip_pad_0);
}

/*
 * test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_frame_0_t
 * is the test's state across its pad: the long keys, the parts of the one
 * being joined, the two key lists and the array that holds them, each list's
 * values, addresses, hashed bytes, and flat bytes, and the corrupted leaf's
 * parts, while live.
 */
typedef struct test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_frame_0_t
{
	StdVec		long_;
	bool		long_live;
	StdVec		parts[2];
	bool		parts_live[2];
	StdVec		varying;
	bool		varying_live;
	StdVec		fixed;
	bool		fixed_live;
	StdVec		lists[2];
	size_t		lists_at;
	bool		lists_live;
	StdVec		keys;
	bool		keys_live;
	StdVec		values;
	bool		values_live;
	StdVec		empty;
	bool		empty_live;
	StdVec		addresses;
	bool		addresses_live;
	StdVec		hashed;
	bool		hashed_live;
	StdVec		flat;
	bool		flat_live;
	StdVec		empties[2];
	bool		empties_live[2];
	ChunksHeaderHeader header;
	bool		header_live;
	StdVec		hashed_2;
	bool		hashed_2_live;
	StdVec		corrupt;
	bool		corrupt_live;
	ChunksErrorError error;
	bool		error_live;
} test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_frame_0_t;

/*
 * test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_body_0
 * is the test: varying keys whose shared prefixes cross every length a
 * prefix takes, and fixed-width keys, round-trip through a leaf, and a
 * leaf whose prefix runs past its key or whose bytes end early is refused.
 */
static void
test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_body_0(void *frame_)
{
	test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_frame_0_t *frame = frame_;
	static const uint8 fixed_0[] = {0, 0, 0, 1};
	static const uint8 fixed_1[] = {0, 0, 0, 2};
	static const uint8 fixed_2[] = {0, 0, 1, 0};
	static const uint8 fixed_3[] = {0, 0, 1, 0};
	static const uint8 fixed_4[] = {1, 2, 3, 4};
	const uint8 *none = STD_DANGLING_FOR(1);
	uint8		ff[300];
	StdSizeHint hint;
	ChunksErrorError error;

	hint.lower = 100;
	hint.has_upper = true;
	hint.upper = 100;
	std_vec_with_capacity_trusted(&frame->long_, hint, sizeof(StdVec), _Alignof(StdVec));
	frame->long_live = true;
	std_vec_extend_trusted_reserve(&frame->long_, hint, sizeof(StdVec), _Alignof(StdVec));
	for (uint8 n = 0, n_end = 100; n < n_end; n = std_forward_unchecked_u8_Checked(n, 1))
	{
		uint8	   *boxed;
		StdVec		joined;

		std_vec_u8_from_elem(&frame->parts[0], 'k', 1000);
		frame->parts_live[0] = true;
		boxed = std_box_new_uninit_layout_Checked(1, 1);
		STD_CHECK_ALIGNED(boxed, 1, 767, 72);
		STD_CHECK_NULL_DEREF(boxed, 767, 72);
		boxed[0] = n;
		std_box_slice_into_vec_Checked(&frame->parts[1], boxed, 1, 1);
		frame->parts_live[1] = true;
		std_slice_concat(frame->parts, 2, sizeof(StdVec), 1, 1, &joined);
		frame->parts_live[0] = false;
		std_vec_free_Checked(&frame->parts[0], 1, 1);
		frame->parts_live[1] = false;
		std_vec_free_Checked(&frame->parts[1], 1, 1);
		memcpy(STD_PTR_AT(frame->long_.ptr, frame->long_.len, sizeof(StdVec)), &joined, sizeof(StdVec));
		frame->long_.len = std_wrapping_add_usize(frame->long_.len, 1);
	}
	memset(ff, 0xff, sizeof(ff));
	{
		StdBytes	items[7] = {{(const uint8 *) "", 0}, {(const uint8 *) "a", 1}, {(const uint8 *) "ab", 2},
		{(const uint8 *) "abc", 3}, {(const uint8 *) "b", 1}, {(const uint8 *) "b\0", 2}, {ff, sizeof(ff)}};
		StdBytes   *boxed = std_box_new_uninit_layout_Checked(sizeof(items), _Alignof(StdBytes));

		STD_CHECK_ALIGNED(boxed, _Alignof(StdBytes), 768, 39);
		STD_CHECK_NULL_DEREF(boxed, 768, 39);
		memcpy(boxed, items, sizeof(items));
		std_box_slice_into_vec_Checked(&frame->varying, boxed, 7, sizeof(StdBytes));
		frame->varying_live = true;
	}
	hint.lower = frame->long_.len;
	hint.has_upper = true;
	hint.upper = frame->long_.len;
	std_vec_extend_trusted_reserve(&frame->varying, hint, sizeof(StdBytes), _Alignof(StdBytes));
	for (size_t k = 0, k_len = frame->long_.len; k < k_len; k++)
	{
		const StdVec *key = &STD_VEC_AT(&frame->long_, StdVec, k);
		StdBytes	slice = {key->ptr, key->len};

		memcpy(STD_PTR_AT(frame->varying.ptr, frame->varying.len, sizeof(StdBytes)), &slice, sizeof(StdBytes));
		frame->varying.len = std_wrapping_add_usize(frame->varying.len, 1);
	}
	{
		StdBytes	items[5] = {{fixed_0, sizeof(fixed_0)}, {fixed_1, sizeof(fixed_1)}, {fixed_2, sizeof(fixed_2)},
		{fixed_3, sizeof(fixed_3)}, {fixed_4, sizeof(fixed_4)}};
		StdBytes   *boxed = std_box_new_uninit_layout_Checked(sizeof(items), _Alignof(StdBytes));

		STD_CHECK_ALIGNED(boxed, _Alignof(StdBytes), 770, 33);
		STD_CHECK_NULL_DEREF(boxed, 770, 33);
		memcpy(boxed, items, sizeof(items));
		std_box_slice_into_vec_Checked(&frame->fixed, boxed, 5, sizeof(StdBytes));
		frame->fixed_live = true;
	}
	frame->lists[0] = frame->varying;
	frame->varying_live = false;
	frame->lists[1] = frame->fixed;
	frame->fixed_live = false;
	frame->lists_live = true;
	for (size_t n; std_array_IntoIter_next(&frame->lists_at, 2, 2, &n);)
	{
		TreesNodeNode node;
		size_t		left;
		size_t		right;

		frame->keys = frame->lists[n];
		frame->keys_live = true;
		hint.lower = frame->keys.len;
		hint.has_upper = true;
		hint.upper = frame->keys.len;
		std_vec_with_capacity_trusted(&frame->values, hint, sizeof(StdBytes), _Alignof(StdBytes));
		frame->values_live = true;
		std_vec_extend_trusted_reserve(&frame->values, hint, sizeof(StdBytes), _Alignof(StdBytes));
		for (size_t k = 0, k_len = frame->keys.len; k < k_len; k++)
		{
			const StdBytes *key = &STD_VEC_AT(&frame->keys, StdBytes, k);
			size_t		end = std_min_usize(key->len, 3);
			StdBytes	value;

			std_range_to_check(end, key->len, RUST_FILE, 772, 60);
			value.ptr = key->ptr;
			value.len = end;
			memcpy(STD_PTR_AT(frame->values.ptr, frame->values.len, sizeof(StdBytes)), &value, sizeof(StdBytes));
			frame->values.len = std_wrapping_add_usize(frame->values.len, 1);
		}
		std_vec_new(&frame->empty, sizeof(uint32), _Alignof(uint32));
		frame->empty_live = true;
		std_vec_with_capacity_Checked(&frame->addresses, frame->keys.len, sizeof(StdVec), _Alignof(StdVec));
		frame->addresses_live = true;
		std_vec_reserve_Checked(&frame->addresses, frame->keys.len, sizeof(StdVec), _Alignof(StdVec));
		for (size_t k = 1, k_end = frame->keys.len; k < k_end; k = std_forward_unchecked_usize_Checked(k, 1))
		{
			std_vec_clone(STD_PTR_AT(frame->addresses.ptr, frame->addresses.len, sizeof(StdVec)), &frame->empty,
						  sizeof(uint32), _Alignof(uint32));
			frame->addresses.len = std_wrapping_add_usize(frame->addresses.len, 1);
		}
		frame->empty_live = false;
		if (frame->keys.len > 0)
		{
			memcpy(STD_PTR_AT(frame->addresses.ptr, frame->addresses.len, sizeof(StdVec)), &frame->empty,
				   sizeof(StdVec));
			frame->addresses.len = std_wrapping_add_usize(frame->addresses.len, 1);
		}
		else
			std_vec_free_Checked(&frame->empty, sizeof(uint32), _Alignof(uint32));
		trees_node_encode_leaf(frame->keys.ptr, frame->keys.len, frame->values.ptr, frame->values.len,
							   frame->addresses.ptr, frame->addresses.len, false, &frame->hashed);
		frame->hashed_live = true;
		frame->addresses_live = false;
		std_vec_drop_Checked(&frame->addresses, sizeof(StdVec), _Alignof(StdVec), trees_node_tests_VecU32_drop);
		if (!trees_node_flat_of(chunks_address_Address_of(frame->hashed.ptr, frame->hashed.len), frame->hashed.ptr,
								frame->hashed.len, none, 0, none, 0, 0, &frame->flat, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(774, 76, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		frame->flat_live = true;
		node = trees_node_Node_view(frame->flat.ptr, frame->flat.len);
		left = trees_node_Node_len(&node);
		right = frame->keys.len;
		STD_ASSERT_EQ(776, 13, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
		for (size_t k = 0, k_len = frame->keys.len, count = 0; k < k_len; k++)
		{
			size_t		i = count;
			const StdBytes *key = &STD_VEC_AT(&frame->keys, StdBytes, k);
			StdBytes	got_key;
			StdBytes	got_value;
			StdBytes	value;

			count = std_Enumerate_count_step(count);
			got_key = trees_node_Node_key(&node, i);
			got_value = trees_node_Node_value(&node, i);
			std_index_check(i, frame->values.len, RUST_FILE, 778, 71);
			value = STD_VEC_AT(&frame->values, StdBytes, i);
			STD_ASSERT_EQ(778, 17, got_key.len == key->len &&
						  memcmp(got_key.ptr, key->ptr, key->len) == 0 && got_value.len == value.len &&
						  memcmp(got_value.ptr, value.ptr, value.len) == 0,
						  &STD_TUPLE(2, STD_ITEM(&got_key, std_bytes_Debug_fmt),
									 STD_ITEM(&got_value, std_bytes_Debug_fmt)), std_tuple_Debug_fmt,
						  &STD_TUPLE(2, STD_ITEM(key, std_bytes_Debug_fmt), STD_ITEM(&value, std_bytes_Debug_fmt)),
						  std_tuple_Debug_fmt);
		}
		frame->flat_live = false;
		std_vec_free_Checked(&frame->flat, 1, 1);
		frame->hashed_live = false;
		std_vec_free_Checked(&frame->hashed, 1, 1);
		frame->values_live = false;
		std_vec_free_Checked(&frame->values, sizeof(StdBytes), _Alignof(StdBytes));
		frame->keys_live = false;
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	}
	frame->lists_live = false;
	{
		StdBytes	keys[2] = {{(const uint8 *) "a", 1}, {(const uint8 *) "ab", 2}};
		StdBytes	values[2] = {{(const uint8 *) "", 0}, {(const uint8 *) "", 0}};

		std_vec_new(&frame->empties[0], sizeof(uint32), _Alignof(uint32));
		frame->empties_live[0] = true;
		std_vec_new(&frame->empties[1], sizeof(uint32), _Alignof(uint32));
		frame->empties_live[1] = true;
		trees_node_encode_leaf(keys, 2, values, 2, frame->empties, 2, false, &frame->hashed_2);
		frame->hashed_2_live = true;
		frame->empties_live[0] = false;
		std_vec_free_Checked(&frame->empties[0], sizeof(uint32), _Alignof(uint32));
		frame->empties_live[1] = false;
		std_vec_free_Checked(&frame->empties[1], sizeof(uint32), _Alignof(uint32));
	}
	{
		size_t		body;
		size_t		at;
		ChunksAddressAddress address;
		size_t		end;
		StdVec		flat;
		bool		ok;

		if (!chunks_header_Header_read(frame->hashed_2.ptr, frame->hashed_2.len, &frame->header, &body, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(782, 40, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		frame->header_live = true;
		at = STD_ADD(usize, body, 2, 782, 18);
		frame->header_live = false;
		chunks_header_Header_drop(&frame->header);
		std_vec_clone(&frame->corrupt, &frame->hashed_2, 1, 1);
		frame->corrupt_live = true;
		std_index_check(at, frame->corrupt.len, RUST_FILE, 784, 16);
		STD_VEC_AT(&frame->corrupt, uint8, at) = 1;
		ok = trees_node_flat_of(chunks_address_Address_of(frame->corrupt.ptr, frame->corrupt.len),
								frame->corrupt.ptr, frame->corrupt.len, none, 0, none, 0, 0, &flat, &error);
		if (ok)
			std_vec_free_Checked(&flat, 1, 1);
		else
			chunks_error_Error_drop(&error);
		STD_ASSERT(785, 9, !ok, "flat_of(Address::of(&corrupt), &corrupt, &[], &[], 0).is_err()");
		address = chunks_address_Address_of(frame->hashed_2.ptr, frame->hashed_2.len);
		end = STD_SUB(usize, frame->hashed_2.len, 6, 786, 57);
		std_range_to_check(end, frame->hashed_2.len, RUST_FILE, 786, 54);
		ok = trees_node_flat_of(address, frame->hashed_2.ptr, end, none, 0, none, 0, 0, &flat, &error);
		if (ok)
			std_vec_free_Checked(&flat, 1, 1);
		else
			chunks_error_Error_drop(&error);
		STD_ASSERT(786, 9, !ok, "flat_of(Address::of(&hashed), &hashed[..hashed.len() - 6], &[], &[], 0).is_err()");
	}
	frame->corrupt_live = false;
	std_vec_free_Checked(&frame->corrupt, 1, 1);
	frame->hashed_2_live = false;
	std_vec_free_Checked(&frame->hashed_2, 1, 1);
	frame->long_live = false;
	std_vec_drop_Checked(&frame->long_, sizeof(StdVec), _Alignof(StdVec), trees_node_tests_VecU8_drop);
}

/*
 * test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_pad_0
 * is the test's pad: an unwrap's error, then every other value its frame
 * holds, in the reverse of the order the test makes them, while live.
 */
static void
test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_pad_0(void *frame_)
{
	test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->corrupt_live)
		std_vec_free_Checked(&frame->corrupt, 1, 1);
	if (frame->hashed_2_live)
		std_vec_free_Checked(&frame->hashed_2, 1, 1);
	if (frame->header_live)
		chunks_header_Header_drop(&frame->header);
	for (size_t k = 0, k_len = 2; k < k_len; k++)
		if (frame->empties_live[k])
			std_vec_free_Checked(&frame->empties[k], sizeof(uint32), _Alignof(uint32));
	if (frame->flat_live)
		std_vec_free_Checked(&frame->flat, 1, 1);
	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
	if (frame->addresses_live)
		std_vec_drop_Checked(&frame->addresses, sizeof(StdVec), _Alignof(StdVec), trees_node_tests_VecU32_drop);
	if (frame->empty_live)
		std_vec_free_Checked(&frame->empty, sizeof(uint32), _Alignof(uint32));
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->lists_live)
		for (size_t k = frame->lists_at, k_len = 2; k < k_len; k++)
			std_vec_free_Checked(&frame->lists[k], sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->fixed_live)
		std_vec_free_Checked(&frame->fixed, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->varying_live)
		std_vec_free_Checked(&frame->varying, sizeof(StdBytes), _Alignof(StdBytes));
	for (size_t k = 0, k_len = 2; k < k_len; k++)
		if (frame->parts_live[k])
			std_vec_free_Checked(&frame->parts[k], 1, 1);
	if (frame->long_live)
		std_vec_drop_Checked(&frame->long_, sizeof(StdVec), _Alignof(StdVec), trees_node_tests_VecU8_drop);
}

/* test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries is the test front_coded_leaf_keys_round_trip_at_prefix_boundaries. */
void
test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries(void)
{
	test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_frame_0_t frame = {0};

	std_landing(&frame, test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_body_0,
				test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries_pad_0);
}

/*
 * test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_frame_0_t
 * is decode's state across its pad: the header it writes and its hashed
 * bytes, while live, its body, and its result.
 */
typedef struct test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_frame_0_t
{
	ChunksHeaderHeader header;
	bool		header_live;
	StdVec		hashed;
	bool		hashed_live;
	const uint8 *body;
	size_t		body_len;
	ChunksErrorError *error;
	bool		ok;
} test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_frame_0_t;

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_body_0 is decode's body. */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_body_0(void *frame_)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_frame_0_t *frame = frame_;
	const uint8 *none = STD_DANGLING_FOR(1);
	StdVec		flat;

	chunks_header_Header_write(&frame->header, frame->body, frame->body_len, &frame->hashed);
	frame->hashed_live = true;
	frame->header_live = false;
	chunks_header_Header_drop(&frame->header);
	frame->ok = trees_node_flat_of(chunks_address_Address_of(frame->hashed.ptr, frame->hashed.len), frame->hashed.ptr,
								   frame->hashed.len, none, 0, none, 0, 0, &flat, frame->error);
	if (frame->ok)
		std_vec_free_Checked(&flat, 1, 1);
	frame->hashed_live = false;
	std_vec_free_Checked(&frame->hashed, 1, 1);
}

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_pad_0 is decode's pad: the hashed bytes and the header, while live. */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_pad_0(void *frame_)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_frame_0_t *frame = frame_;

	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
	if (frame->header_live)
		chunks_header_Header_drop(&frame->header);
}

/*
 * test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0
 * is the test's decode: a node of the kind and flags with the address
 * offsets, which it takes, over the body, decoded or refused.
 */
static bool
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(ChunksHeaderKind kind, uint8 flags, StdVec *offsets, const uint8 *body, size_t body_len,
																					 ChunksErrorError *error)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_frame_0_t frame = {.body = body, .body_len = body_len, .error = error};

	frame.header.kind = kind;
	frame.header.flags = flags;
	frame.header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
	frame.header.addresses.u.offsets = *offsets;
	frame.header_live = true;
	std_landing(&frame, test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_body_0, test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0_pad_0);
	return frame.ok;
}

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_1 is the test's leaf: a leaf body of two keys with the value offsets. */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_1(const uint8 *offsets, StdVec *result)
{
	static const uint8 head[] = {2, 1, 0, 'a', 0, 'b'};
	StdBytes	parts[3] = {{head, sizeof(head)}, {offsets, 6}, {(const uint8 *) "xy", 2}};

	std_slice_concat(parts, 3, sizeof(StdBytes), 1, 1, result);
}

/*
 * test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_2
 * is the test's internal: an internal body of two keys with the key offsets,
 * the counts' bytes, and the summaries' bytes.
 */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_2(const uint8 *offsets, const uint8 *counts, size_t counts_len, const uint8 *summaries,
																					 size_t summaries_len, StdVec *result)
{
	static const uint8 head[] = {1, 2, 0};
	uint8		sevens[64];
	StdBytes	parts[6];

	memset(sevens, 7, sizeof(sevens));
	parts[0].ptr = head;
	parts[0].len = sizeof(head);
	parts[1].ptr = offsets;
	parts[1].len = 6;
	parts[2].ptr = (const uint8 *) "k";
	parts[2].len = 1;
	parts[3].ptr = sevens;
	parts[3].len = sizeof(sevens);
	parts[4].ptr = counts;
	parts[4].len = counts_len;
	parts[5].ptr = summaries;
	parts[5].len = summaries_len;
	std_slice_concat(parts, 6, sizeof(StdBytes), 1, 1, result);
}

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_frame_0_t is summaries' state across its pad: its offsets, the summaries' bytes while live, and its result. */
typedef struct test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_frame_0_t
{
	const uint8 *offsets;
	StdVec		joined;
	bool		joined_live;
	StdVec	   *result;
} test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_frame_0_t;

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_body_0 is summaries' body. */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_body_0(void *frame_)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_frame_0_t *frame = frame_;
	static const uint8 offsets[6] = {0, 0, 1, 0, 1, 0};
	static const uint8 counts[2] = {1, 1};
	StdBytes	parts[2] = {{frame->offsets, 6}, {(const uint8 *) "s", 1}};

	std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, &frame->joined);
	frame->joined_live = true;
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_2(offsets, counts, sizeof(counts), frame->joined.ptr, frame->joined.len, frame->result);
	frame->joined_live = false;
	std_vec_free_Checked(&frame->joined, 1, 1);
}

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_pad_0 is summaries' pad: the summaries' bytes, while live. */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_pad_0(void *frame_)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_frame_0_t *frame = frame_;

	if (frame->joined_live)
		std_vec_free_Checked(&frame->joined, 1, 1);
}

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3 is the test's summaries: an internal body whose summaries have the offsets. */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3(const uint8 *offsets, StdVec *result)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_frame_0_t frame = {.offsets = offsets, .result = result};

	std_landing(&frame, test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_body_0, test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3_pad_0);
}

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_frame_0_t is the test's state across its pad: the offsets and body bytes of the call being made, and the addressed leaf's body, while live. */
typedef struct test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_frame_0_t
{
	StdVec		offsets;
	bool		offsets_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		addressed;
	bool		addressed_live;
} test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_frame_0_t;

/*
 * test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_body_0
 * is the test: a node whose value, key, count, summary, or address offsets
 * point past what its accessors read is refused, and one whose offsets fit is
 * decoded.
 */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_body_0(void *frame_)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_frame_0_t *frame = frame_;
	static const uint8 overflowing[11] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x01};
	static const uint8 ones[2] = {1, 1};
	const uint8 *none = STD_DANGLING_FOR(1);
	ChunksErrorError error;
	bool		ok;

	std_vec_new(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	frame->offsets_live = true;
	{
		static const uint8 offsets[6] = {0, 0, 1, 0, 2, 0};

		test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_1(offsets, &frame->bytes);
	}
	frame->bytes_live = true;
	frame->offsets_live = false;
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_LEAF, 0, &frame->offsets, frame->bytes.ptr, frame->bytes.len, &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	STD_ASSERT(796, 9, ok, "decode(TREE_LEAF, 0, vec![], &leaf([0, 0, 1, 0, 2, 0])).is_ok()");
	std_vec_new(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	frame->offsets_live = true;
	{
		static const uint8 offsets[6] = {0, 0, 3, 0, 2, 0};

		test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_1(offsets, &frame->bytes);
	}
	frame->bytes_live = true;
	frame->offsets_live = false;
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_LEAF, 0, &frame->offsets, frame->bytes.ptr, frame->bytes.len, &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	STD_ASSERT(797, 9, !ok, "decode(TREE_LEAF, 0, vec![], &leaf([0, 0, 3, 0, 2, 0])).is_err()");
	std_vec_new(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	frame->offsets_live = true;
	{
		static const uint8 offsets[6] = {0, 0, 1, 0, 1, 0};

		test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_2(offsets, ones, sizeof(ones), none, 0, &frame->bytes);
	}
	frame->bytes_live = true;
	frame->offsets_live = false;
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_INTERNAL, 0, &frame->offsets, frame->bytes.ptr, frame->bytes.len, &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	STD_ASSERT(801, 9, ok, "decode(TREE_INTERNAL, 0, vec![], &internal([0, 0, 1, 0, 1, 0], &[1, 1], &[])).is_ok()");
	std_vec_new(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	frame->offsets_live = true;
	{
		static const uint8 offsets[6] = {0, 0, 2, 0, 1, 0};

		test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_2(offsets, ones, sizeof(ones), none, 0, &frame->bytes);
	}
	frame->bytes_live = true;
	frame->offsets_live = false;
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_INTERNAL, 0, &frame->offsets, frame->bytes.ptr, frame->bytes.len, &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	STD_ASSERT(802, 9, !ok, "decode(TREE_INTERNAL, 0, vec![], &internal([0, 0, 2, 0, 1, 0], &[1, 1], &[])).is_err()");
	std_vec_new(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	frame->offsets_live = true;
	{
		static const uint8 offsets[6] = {0, 0, 1, 0, 1, 0};

		test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_2(offsets, overflowing, sizeof(overflowing), none, 0, &frame->bytes);
	}
	frame->bytes_live = true;
	frame->offsets_live = false;
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_INTERNAL, 0, &frame->offsets, frame->bytes.ptr, frame->bytes.len, &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	STD_ASSERT(804, 9, !ok, "decode(TREE_INTERNAL, 0, vec![], &internal([0, 0, 1, 0, 1, 0], &overflowing, &[])).is_err()");
	std_vec_new(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	frame->offsets_live = true;
	{
		static const uint8 offsets[6] = {0, 0, 1, 0, 1, 0};

		test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3(offsets, &frame->bytes);
	}
	frame->bytes_live = true;
	frame->offsets_live = false;
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_INTERNAL, TREES_NODE_SUMMARIES, &frame->offsets, frame->bytes.ptr, frame->bytes.len, &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	STD_ASSERT(806, 9, ok, "decode(TREE_INTERNAL, SUMMARIES, vec![], &summaries([0, 0, 1, 0, 1, 0])).is_ok()");
	std_vec_new(&frame->offsets, sizeof(uint32), _Alignof(uint32));
	frame->offsets_live = true;
	{
		static const uint8 offsets[6] = {0, 0, 2, 0, 1, 0};

		test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_3(offsets, &frame->bytes);
	}
	frame->bytes_live = true;
	frame->offsets_live = false;
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_INTERNAL, TREES_NODE_SUMMARIES, &frame->offsets, frame->bytes.ptr, frame->bytes.len, &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	STD_ASSERT(807, 9, !ok, "decode(TREE_INTERNAL, SUMMARIES, vec![], &summaries([0, 0, 2, 0, 1, 0])).is_err()");
	{
		static const uint8 head[] = {1, 1, 0, 'a', 0, 0, 64, 0};
		static const uint8 zeros[64] = {0};
		StdBytes	parts[2] = {{head, sizeof(head)}, {zeros, sizeof(zeros)}};

		std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, &frame->addressed);
		frame->addressed_live = true;
	}
	{
		uint32	   *boxed = std_box_new_uninit_layout_Checked(2 * sizeof(uint32), _Alignof(uint32));

		STD_CHECK_ALIGNED(boxed, _Alignof(uint32), 809, 38);
		STD_CHECK_NULL_DEREF(boxed, 809, 38);
		boxed[0] = 8;
		boxed[1] = 40;
		std_box_slice_into_vec_Checked(&frame->offsets, boxed, 2, sizeof(uint32));
	}
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_LEAF, 0, &frame->offsets, frame->addressed.ptr, frame->addressed.len,
																							  &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	STD_ASSERT(809, 9, ok, "decode(TREE_LEAF, 0, vec![8, 40], &addressed).is_ok()");
	{
		uint32	   *boxed = std_box_new_uninit_layout_Checked(2 * sizeof(uint32), _Alignof(uint32));

		STD_CHECK_ALIGNED(boxed, _Alignof(uint32), 810, 38);
		STD_CHECK_NULL_DEREF(boxed, 810, 38);
		boxed[0] = 40;
		boxed[1] = 8;
		std_box_slice_into_vec_Checked(&frame->offsets, boxed, 2, sizeof(uint32));
	}
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_LEAF, 0, &frame->offsets, frame->addressed.ptr, frame->addressed.len,
																							  &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	STD_ASSERT(810, 9, !ok, "decode(TREE_LEAF, 0, vec![40, 8], &addressed).is_err()");
	{
		uint32	   *boxed = std_box_new_uninit_layout_Checked(1 * sizeof(uint32), _Alignof(uint32));

		STD_CHECK_ALIGNED(boxed, _Alignof(uint32), 811, 38);
		STD_CHECK_NULL_DEREF(boxed, 811, 38);
		boxed[0] = 0;
		std_box_slice_into_vec_Checked(&frame->offsets, boxed, 1, sizeof(uint32));
	}
	ok = test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_closure_0(CHUNKS_HEADER_TREE_LEAF, 0, &frame->offsets, frame->addressed.ptr, frame->addressed.len,
																							  &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	STD_ASSERT(811, 9, !ok, "decode(TREE_LEAF, 0, vec![0], &addressed).is_err()");
	frame->addressed_live = false;
	std_vec_free_Checked(&frame->addressed, 1, 1);
}

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_pad_0 is the test's pad: the addressed leaf's body, the body bytes, and the offsets, while live. */
static void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_pad_0(void *frame_)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_frame_0_t *frame = frame_;

	if (frame->addressed_live)
		std_vec_free_Checked(&frame->addressed, 1, 1);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, sizeof(uint32), _Alignof(uint32));
}

/* test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read is the test refuses_offsets_and_counts_its_accessors_cannot_read. */
void
test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read(void)
{
	test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_frame_0_t frame = {0};

	std_landing(&frame, test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_body_0, test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read_pad_0);
}

#endif							/* DOLTGRES_TEST_TREES */
