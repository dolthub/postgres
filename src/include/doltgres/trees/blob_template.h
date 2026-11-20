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
 * The template of trees' blob_tree<C, E> and internal<C, E> (trees' blob.rs),
 * one instance per (C, E), since each returns a Result that holds E by value
 * (the port text's section 2); C, reached only by &mut, is a pointer. Before
 * including it, in blob.c, define:
 *
 * TREES_BLOB_CE: (C, E)'s spelling in the instance's names.
 * TREES_BLOB_CX: C's C type, which the instance points to.
 * TREES_BLOB_FALLIBLE: 1 for an E with values, whose instance returns bool
 * and takes section 5's error pointer, and 0 for Infallible, whose instance
 * has no error at all.
 *
 * It undefines every name it takes at its end.
 */

#include "doltgres/chunks/header.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/node.h"

/* TREES_BLOB_CAT pastes two tokens after expanding them. */
#define TREES_BLOB_CAT_(a, b) a##b
#define TREES_BLOB_CAT(a, b) TREES_BLOB_CAT_(a, b)

/* TREES_BLOB_NAME names the instance's function or type after (C, E). */
#define TREES_BLOB_NAME(name) TREES_BLOB_CAT(name##_, TREES_BLOB_CE)

#if TREES_BLOB_FALLIBLE
#define TREES_BLOB_RETURNS bool
#define TREES_BLOB_ERROR_PARAM , ChunksErrorError *error
#else
#define TREES_BLOB_RETURNS void
#define TREES_BLOB_ERROR_PARAM
#endif

/*
 * The internal instance's frame is its state across its pad: its arguments
 * and the node's keys, addresses, counts, locators, and stamps, while live.
 */
typedef struct TREES_BLOB_NAME (trees_blob_internal_frame_0_t)
{
	TREES_BLOB_CX *cx;

	TREES_BLOB_RETURNS(*write) (void *captures, TREES_BLOB_CX * cx, StdVec *hashed, const ChunksTrailerLocator *locators,
								size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTree *result
								TREES_BLOB_ERROR_PARAM);
	void	   *write_captures;
	uint8		level;
	const trees_TupleTreeU64_t *children;
	size_t		children_len;
	trees_TupleTreeU64_t *result;
#if TREES_BLOB_FALLIBLE
	ChunksErrorError *error;
	bool		ok;
#endif
	StdVec		keys;
	bool		keys_live;
	StdVec		addresses;
	bool		addresses_live;
	StdVec		counts;
	bool		counts_live;
	StdVec		locators;
	bool		locators_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
} TREES_BLOB_NAME(trees_blob_internal_frame_0_t);

/* The internal instance's pad: the stamps, locators, counts, addresses, and keys, while live. */
static void
TREES_BLOB_NAME(trees_blob_internal_pad_0) (void *frame_)
{
	TREES_BLOB_NAME(trees_blob_internal_frame_0_t) * frame = frame_;

	if (frame->stamps_live)
		chunks_trailer_Stamps_drop(&frame->stamps);
	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	if (frame->counts_live)
		std_vec_free_Checked(&frame->counts, sizeof(uint64), _Alignof(uint64));
	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
}

/*
 * The internal instance's body: the node's empty keys, its children's
 * addresses, counts, and locators, and its stamps of zero tuple ids, the
 * encoded node handed to write, and its length summed.
 */
static void
TREES_BLOB_NAME(trees_blob_internal_body_0) (void *frame_)
{
	TREES_BLOB_NAME(trees_blob_internal_frame_0_t) * frame = frame_;
	size_t		n = frame->children_len;
	StdBytes	empty = {STD_DANGLING_FOR(1), 0};
	StdSizeHint hint = {n, true, n};
	StdVec		zeros;
	StdVec		hashed;
	TreesStoreTree tree;
	uint64		total = 0;

	std_vec_with_capacity_Checked(&frame->keys, n, sizeof(StdBytes), _Alignof(StdBytes));
	frame->keys_live = true;
	std_vec_resize(&frame->keys, n, &empty, sizeof(StdBytes), _Alignof(StdBytes));
	std_vec_with_capacity_trusted(&frame->addresses, hint, sizeof(ChunksAddressAddress),
								  _Alignof(ChunksAddressAddress));
	frame->addresses_live = true;
	std_vec_extend_trusted_reserve(&frame->addresses, hint, sizeof(ChunksAddressAddress),
								   _Alignof(ChunksAddressAddress));
	for (size_t k = 0, k_len = n; k < k_len; k++)
	{
		STD_VEC_AT(&frame->addresses, ChunksAddressAddress, frame->addresses.len) = frame->children[k].f0.address;
		frame->addresses.len = std_wrapping_add_usize(frame->addresses.len, 1);
	}
	std_vec_with_capacity_trusted(&frame->counts, hint, sizeof(uint64), _Alignof(uint64));
	frame->counts_live = true;
	std_vec_extend_trusted_reserve(&frame->counts, hint, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0, k_len = n; k < k_len; k++)
	{
		STD_VEC_AT(&frame->counts, uint64, frame->counts.len) = frame->children[k].f1;
		frame->counts.len = std_wrapping_add_usize(frame->counts.len, 1);
	}
	std_vec_with_capacity_trusted(&frame->locators, hint, sizeof(ChunksTrailerLocator),
								  _Alignof(ChunksTrailerLocator));
	frame->locators_live = true;
	std_vec_extend_trusted_reserve(&frame->locators, hint, sizeof(ChunksTrailerLocator),
								   _Alignof(ChunksTrailerLocator));
	for (size_t k = 0, k_len = n; k < k_len; k++)
	{
		STD_VEC_AT(&frame->locators, ChunksTrailerLocator, frame->locators.len) = frame->children[k].f0.locator;
		frame->locators.len = std_wrapping_add_usize(frame->locators.len, 1);
	}
	std_vec_from_elem_zeroed(&zeros, n, sizeof(uint64), _Alignof(uint64));
	frame->stamps = chunks_trailer_Stamps_subtrees(&zeros);
	frame->stamps_live = true;
	trees_node_encode_internal(frame->level, frame->keys.ptr, frame->keys.len, frame->addresses.ptr,
							   frame->addresses.len, frame->counts.ptr, frame->counts.len, &hashed);
#if TREES_BLOB_FALLIBLE
	if (!frame->write(frame->write_captures, frame->cx, &hashed, frame->locators.ptr, frame->locators.len,
					  &frame->stamps, &tree, frame->error))
	{
		TREES_BLOB_NAME(trees_blob_internal_pad_0) (frame);
		frame->ok = false;
		return;
	}
#else
	frame->write(frame->write_captures, frame->cx, &hashed, frame->locators.ptr, frame->locators.len, &frame->stamps,
				 &tree);
#endif
	for (size_t k = 0, k_len = frame->counts.len; k < k_len; k++)
		total = std_add_u64(total, STD_VEC_AT(&frame->counts, uint64, k), STD_ACCUM_FILE, 206, 1);
	frame->result->f0 = tree;
	frame->result->f1 = total;
	TREES_BLOB_NAME(trees_blob_internal_pad_0) (frame);
#if TREES_BLOB_FALLIBLE
	frame->ok = true;
#endif
}

/*
 * The internal instance makes a blob tree's internal node over the children,
 * hands it to write with the context it writes in, and returns it with its
 * length.
 */
static TREES_BLOB_RETURNS
TREES_BLOB_NAME(trees_blob_internal) (TREES_BLOB_CX * cx,
									  TREES_BLOB_RETURNS(*write) (void *captures, TREES_BLOB_CX * cx, StdVec *hashed,
																  const ChunksTrailerLocator *locators,
																  size_t locators_len,
																  const ChunksTrailerStamps *stamps,
																  TreesStoreTree *result TREES_BLOB_ERROR_PARAM),
									  void *write_captures, uint8 level, const trees_TupleTreeU64_t *children,
									  size_t children_len, trees_TupleTreeU64_t *result TREES_BLOB_ERROR_PARAM)
{
	TREES_BLOB_NAME(trees_blob_internal_frame_0_t) frame =
	{
		.cx = cx, .write = write,
			.write_captures = write_captures, .level = level, .children = children, .children_len = children_len,
			.result = result
	};

#if TREES_BLOB_FALLIBLE
	frame.error = error;
#endif
	std_landing(&frame, TREES_BLOB_NAME(trees_blob_internal_body_0), TREES_BLOB_NAME(trees_blob_internal_pad_0));
#if TREES_BLOB_FALLIBLE
	return frame.ok;
#endif
}

/*
 * The blob_tree instance's frame is its state across its pad: its arguments,
 * the level's children, its splitter, and the parents it makes, while live.
 */
typedef struct TREES_BLOB_NAME (trees_blob_blob_tree_frame_0_t)
{
	TREES_BLOB_CX *cx;
	const uint8 *data;
	size_t		data_len;
	uint32		target;

	TREES_BLOB_RETURNS(*write) (void *captures, TREES_BLOB_CX * cx, StdVec *hashed, const ChunksTrailerLocator *locators,
								size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTree *result
								TREES_BLOB_ERROR_PARAM);
	void	   *write_captures;
	TreesStoreTree *result;
#if TREES_BLOB_FALLIBLE
	ChunksErrorError *error;
	bool		ok;
#endif
	StdVec		children;
	bool		children_live;
	TreesChunkerSplitter splitter;
	bool		splitter_live;
	StdVec		parents;
	bool		parents_live;
} TREES_BLOB_NAME(trees_blob_blob_tree_frame_0_t);

/* The blob_tree instance's pad: the parents, the splitter, and the children, while live. */
static void
TREES_BLOB_NAME(trees_blob_blob_tree_pad_0) (void *frame_)
{
	TREES_BLOB_NAME(trees_blob_blob_tree_frame_0_t) * frame = frame_;

	if (frame->parents_live)
		std_vec_free_Checked(&frame->parents, sizeof(trees_TupleTreeU64_t), _Alignof(trees_TupleTreeU64_t));
	if (frame->splitter_live)
		trees_chunker_Splitter_drop(&frame->splitter);
	if (frame->children_live)
		std_vec_free_Checked(&frame->children, sizeof(trees_TupleTreeU64_t), _Alignof(trees_TupleTreeU64_t));
}

/*
 * The blob_tree instance's body: the bytes cut into leaves, each handed to
 * write, then each level's children split into parents until one is left.
 */
static void
TREES_BLOB_NAME(trees_blob_blob_tree_body_0) (void *frame_)
{
	TREES_BLOB_NAME(trees_blob_blob_tree_frame_0_t) * frame = frame_;
	const uint8 *rest = frame->data;
	size_t		rest_len = frame->data_len;
	uint8		level = 1;

	std_vec_new(&frame->children, sizeof(trees_TupleTreeU64_t), _Alignof(trees_TupleTreeU64_t));
	frame->children_live = true;
	for (;;)
	{
		size_t		len = trees_blob_cut(rest, rest_len);
		ChunksHeaderHeader header;
		StdVec		hashed;
		ChunksTrailerStamps stamps;
		trees_TupleTreeU64_t child;

		header.kind = CHUNKS_HEADER_BLOB_LEAF;
		header.flags = 0;
		header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
		std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
		std_range_to_check(len, rest_len, RUST_FILE, 108, 104);
		chunks_header_Header_write(&header, rest, len, &hashed);
		chunks_header_Header_drop(&header);
		stamps = chunks_trailer_Stamps_Default_default();
#if TREES_BLOB_FALLIBLE
		if (!frame->write(frame->write_captures, frame->cx, &hashed,
						  STD_DANGLING_FOR(_Alignof(ChunksTrailerLocator)), 0, &stamps, &child.f0, frame->error))
		{
			chunks_trailer_Stamps_drop(&stamps);
			TREES_BLOB_NAME(trees_blob_blob_tree_pad_0) (frame);
			frame->ok = false;
			return;
		}
#else
		frame->write(frame->write_captures, frame->cx, &hashed, STD_DANGLING_FOR(_Alignof(ChunksTrailerLocator)), 0,
					 &stamps, &child.f0);
#endif
		child.f1 = (uint64) len;
		std_vec_push_Checked(&frame->children, &child, sizeof(child), _Alignof(trees_TupleTreeU64_t));
		chunks_trailer_Stamps_drop(&stamps);
		std_range_from_check(len, rest_len, RUST_FILE, 110, 21);
		rest = STD_PTR_AT(rest, len, 1);
		rest_len = std_wrapping_sub_usize(rest_len, len);
		if (rest_len == 0)
			break;
	}
	while (frame->children.len > 1)
	{
		size_t		start = 0;

		frame->splitter = trees_chunker_Splitter_new(level, frame->target);
		frame->splitter_live = true;
		std_vec_new(&frame->parents, sizeof(trees_TupleTreeU64_t), _Alignof(trees_TupleTreeU64_t));
		frame->parents_live = true;
		for (size_t i = 0, end = frame->children.len; i < end; i = std_forward_unchecked_usize_Checked(i, 1))
		{
			const trees_TupleTreeU64_t *child;

			std_index_check(i, frame->children.len, RUST_FILE, 121, 38);
			child = &STD_VEC_AT(&frame->children, trees_TupleTreeU64_t, i);
			if (trees_chunker_Splitter_add(&frame->splitter, child->f0.address.f0, CHUNKS_ADDRESS_ADDRESS_LEN, 40) ||
				STD_ADD(usize, i, 1, 121, 62) == frame->children.len)
			{
				trees_TupleTreeU64_t parent;

				std_range_inclusive_check(start, i, frame->children.len, RUST_FILE, 122, 71);
#if TREES_BLOB_FALLIBLE
				if (!TREES_BLOB_NAME(trees_blob_internal) (frame->cx, frame->write, frame->write_captures, level,
														   &STD_VEC_AT(&frame->children, trees_TupleTreeU64_t, start),
														   std_wrapping_sub_usize(std_wrapping_add_usize(i, 1), start),
														   &parent, frame->error))
				{
					TREES_BLOB_NAME(trees_blob_blob_tree_pad_0) (frame);
					frame->ok = false;
					return;
				}
#else
				TREES_BLOB_NAME(trees_blob_internal) (frame->cx, frame->write, frame->write_captures, level,
													  &STD_VEC_AT(&frame->children, trees_TupleTreeU64_t, start),
													  std_wrapping_sub_usize(std_wrapping_add_usize(i, 1), start),
													  &parent);
#endif
				std_vec_push_Checked(&frame->parents, &parent, sizeof(parent), _Alignof(trees_TupleTreeU64_t));
				start = STD_ADD(usize, i, 1, 123, 25);
			}
		}
		std_vec_free_Checked(&frame->children, sizeof(trees_TupleTreeU64_t), _Alignof(trees_TupleTreeU64_t));
		frame->children = frame->parents;
		frame->parents_live = false;
		level = STD_ADD(u8, level, 1, 127, 9);
		frame->splitter_live = false;
		trees_chunker_Splitter_drop(&frame->splitter);
	}
	std_index_check(0, frame->children.len, RUST_FILE, 129, 16);
	*frame->result = STD_VEC_AT(&frame->children, trees_TupleTreeU64_t, 0).f0;
	frame->children_live = false;
	std_vec_free_Checked(&frame->children, sizeof(trees_TupleTreeU64_t), _Alignof(trees_TupleTreeU64_t));
#if TREES_BLOB_FALLIBLE
	frame->ok = true;
#endif
}

/*
 * The blob_tree instance makes the bytes' blob tree with internal nodes of
 * the target size, each node's hashed bytes, with its children's locators and
 * stamps, handed to write with the context it writes in, and returns its
 * root.
 */
static TREES_BLOB_RETURNS
TREES_BLOB_NAME(trees_blob_blob_tree) (TREES_BLOB_CX * cx, const uint8 *data, size_t data_len, uint32 target,
									   TREES_BLOB_RETURNS(*write) (void *captures, TREES_BLOB_CX * cx, StdVec *hashed,
																   const ChunksTrailerLocator *locators,
																   size_t locators_len,
																   const ChunksTrailerStamps *stamps,
																   TreesStoreTree *result TREES_BLOB_ERROR_PARAM),
									   void *write_captures, TreesStoreTree *result TREES_BLOB_ERROR_PARAM)
{
	TREES_BLOB_NAME(trees_blob_blob_tree_frame_0_t) frame =
	{
		.cx = cx, .data = data, .data_len = data_len,
			.target = target, .write = write, .write_captures = write_captures, .result = result
	};

#if TREES_BLOB_FALLIBLE
	frame.error = error;
#endif
	std_landing(&frame, TREES_BLOB_NAME(trees_blob_blob_tree_body_0), TREES_BLOB_NAME(trees_blob_blob_tree_pad_0));
#if TREES_BLOB_FALLIBLE
	return frame.ok;
#endif
}

#undef TREES_BLOB_CAT_
#undef TREES_BLOB_CAT
#undef TREES_BLOB_NAME
#undef TREES_BLOB_RETURNS
#undef TREES_BLOB_ERROR_PARAM
#undef TREES_BLOB_CE
#undef TREES_BLOB_CX
#undef TREES_BLOB_FALLIBLE
