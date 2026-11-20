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
 * The C port of trees' blob.rs: blob trees, whose leaves are raw bytes cut
 * where FastCDC with a fixed gear table says, and whose internal nodes hold
 * each child's address and length and end where the hash of a child's
 * address says, so both depend only on the content.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/header.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/blob.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/node.h"

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "blob.rs"

/* TREES_BLOB_MIN_LEAF is the smallest leaf, except a blob's last. */
#define TREES_BLOB_MIN_LEAF ((size_t) 2 << 10)

/* TREES_BLOB_AVG_LEAF is the leaf size the cut points aim for. */
#define TREES_BLOB_AVG_LEAF ((size_t) 8 << 10)

/* TREES_BLOB_MASK_SMALL is the cut mask below the average size, which makes a cut less likely there. */
#define TREES_BLOB_MASK_SMALL (((UINT64CONST(1) << 15) - 1) << 49)

/* TREES_BLOB_MASK_LARGE is the cut mask above the average size, which makes a cut more likely there. */
#define TREES_BLOB_MASK_LARGE (((UINT64CONST(1) << 11) - 1) << 53)

/* trees_TupleTreeU64_t is (Tree, u64): a blob subtree's root and its length in bytes. */
typedef struct trees_TupleTreeU64_t
{
	TreesStoreTree f0;
	uint64		f1;
} trees_TupleTreeU64_t;

static bool trees_blob_slice_into(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree, uint64 offset,
								  uint8 *out, size_t out_len, size_t *result, ChunksErrorError *error);
static bool trees_blob_read_into(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree, StdVec *out,
								 ChunksErrorError *error);

/* TREES_BLOB_GEAR_GEAR is gear's GEAR, the OnceLock of the gear table. */
static StdOnceLock TREES_BLOB_GEAR_GEAR = STD_ONCE_LOCK_NEW;

/* TREES_BLOB_GEAR_GEAR_VALUE is TREES_BLOB_GEAR_GEAR's value. */
static uint64 TREES_BLOB_GEAR_GEAR_VALUE[256];

/*
 * trees_blob_gear_closure_0 is get_or_init's closure in gear: the table's
 * 256 numbers from SplitMix64 with a fixed seed, array::from_fn's closure
 * run for each in turn.
 */
static void
trees_blob_gear_closure_0(void *captures, void *result)
{
	uint64	   *gear = result;
	uint64		state = UINT64CONST(0x6a09e667f3bcc908);

	for (size_t k = 0, k_len = 256; k < k_len; k++)
	{
		uint64		z;

		state = std_wrapping_add_u64(state, UINT64CONST(0x9e3779b97f4a7c15));
		z = state;
		z = std_wrapping_mul_u64(z ^ STD_SHR_FOLDED(u64, z, 30), UINT64CONST(0xbf58476d1ce4e5b9));
		z = std_wrapping_mul_u64(z ^ STD_SHR_FOLDED(u64, z, 27), UINT64CONST(0x94d049bb133111eb));
		gear[k] = z ^ STD_SHR_FOLDED(u64, z, 31);
	}
}

/* trees_blob_gear returns the gear table, fixed numbers from SplitMix64 with a fixed seed. */
static const uint64 *
trees_blob_gear(void)
{
	std_once_lock_get_or_init(&TREES_BLOB_GEAR_GEAR, TREES_BLOB_GEAR_GEAR_VALUE, trees_blob_gear_closure_0, NULL);
	return TREES_BLOB_GEAR_GEAR_VALUE;
}

/*
 * trees_blob_cut returns the length of the first leaf of the bytes; the
 * enumerated walk skips to MIN_LEAF with Enumerate::nth's counts, then steps
 * with Enumerate::next's.
 */
static size_t
trees_blob_cut(const uint8 *data, size_t data_len)
{
	const uint64 *gear;
	size_t		end;
	uint64		hash = 0;
	size_t		count = 0;

	if (data_len <= TREES_BLOB_MIN_LEAF)
		return data_len;
	gear = trees_blob_gear();
	end = std_min_usize(data_len, TREES_BLOB_MAX_LEAF);
	for (size_t k = 0, k_len = std_wrapping_sub_usize(end, TREES_BLOB_MIN_LEAF); k < k_len; k++)
	{
		size_t		i;
		uint64		mask;

		if (k == 0)
		{
			i = std_add_usize(count, TREES_BLOB_MIN_LEAF, STD_ENUMERATE_FILE, 95, 17);
			count = std_add_usize(i, 1, STD_ENUMERATE_FILE, 96, 22);
		}
		else
		{
			i = count;
			count = std_Enumerate_count_step(count);
		}
		std_index_check(data[i], 256, RUST_FILE, 69, 41);
		hash = std_wrapping_add_u64(STD_SHL_FOLDED(u64, hash, 1), gear[data[i]]);
		mask = i < TREES_BLOB_AVG_LEAF ? TREES_BLOB_MASK_SMALL : TREES_BLOB_MASK_LARGE;
		if ((hash & mask) == 0)
			return STD_ADD(usize, i, 1, 72, 20);
	}
	return end;
}

/*
 * trees_blob_write_blob_closure_0 is write_blob's closure: the node written
 * to the store, as the tree it names.
 */
static bool
trees_blob_write_blob_closure_0(void *captures, HostIoIo *cx, StdVec *hashed, const ChunksTrailerLocator *locators,
								size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTree *result,
								ChunksErrorError *error)
{
	TreesStoreNodeStore *store = captures;
	TreesStoreTreeRef written;

	if (!store->ops->write(store->self, cx, hashed, locators, locators_len, stamps, &written, error))
		return false;
	*result = trees_store_TreeRef_tree(&written);
	return true;
}

/* blob_tree's and internal's instance at (Io, Error), write_blob's. */
#define TREES_BLOB_CE IoError
#define TREES_BLOB_CX HostIoIo
#define TREES_BLOB_FALLIBLE 1
#include "doltgres/trees/blob_template.h"

/* blob_tree's and internal's instance at ((), Infallible), blob_name's. */
#define TREES_BLOB_CE UnitInfallible
#define TREES_BLOB_CX void
#define TREES_BLOB_FALLIBLE 0
#include "doltgres/trees/blob_template.h"

/* trees_blob_write_blob writes the bytes as a blob tree with internal nodes of the target size. */
bool
trees_blob_write_blob(HostIoIo *io, TreesStoreNodeStore store, const uint8 *data, size_t data_len, uint32 target,
					  TreesStoreTreeRef *result, ChunksErrorError *error)
{
	TreesStoreTree tree;

	if (!trees_blob_blob_tree_IoError(io, data, data_len, target, trees_blob_write_blob_closure_0, &store, &tree,
									  error))
		return false;
	*result = trees_store_TreeRef_of(tree);
	return true;
}

/*
 * trees_blob_blob_name_closure_0 is blob_name's closure: the tree a node's
 * hashed bytes would be written as, at the default locator.
 */
static void
trees_blob_blob_name_closure_0(void *captures, void *cx, StdVec *hashed, const ChunksTrailerLocator *locators,
							   size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTree *result)
{
	*result = trees_store_Tree_at(chunks_address_Address_of(hashed->ptr, hashed->len),
								  chunks_trailer_Locator_Default_default());
	std_vec_free_Checked(hashed, 1, 1);
}

/*
 * trees_blob_blob_name returns the address write_blob would give the bytes'
 * blob tree at the target size, writing nothing, so a key can name a blob its
 * value would be written as.
 */
ChunksAddressAddress
trees_blob_blob_name(const uint8 *data, size_t data_len, uint32 target)
{
	TreesStoreTree tree;

	trees_blob_blob_tree_UnitInfallible(NULL, data, data_len, target, trees_blob_blob_name_closure_0, NULL, &tree);
	return tree.address;
}

/* trees_blob_read_blob_frame_0_t is read_blob's state across its pad: the bytes read so far, while live. */
typedef struct trees_blob_read_blob_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreTree tree;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		out;
	bool		out_live;
} trees_blob_read_blob_frame_0_t;

/* trees_blob_read_blob_body_0 is read_blob's body: the tree's bytes appended to an empty vector. */
static void
trees_blob_read_blob_body_0(void *frame_)
{
	trees_blob_read_blob_frame_0_t *frame = frame_;

	std_vec_new(&frame->out, 1, 1);
	frame->out_live = true;
	if (!trees_blob_read_into(frame->io, frame->store, frame->tree, &frame->out, frame->error))
	{
		frame->out_live = false;
		std_vec_free_Checked(&frame->out, 1, 1);
		frame->ok = false;
		return;
	}
	frame->out_live = false;
	*frame->result = frame->out;
	frame->ok = true;
}

/* trees_blob_read_blob_pad_0 is read_blob's pad: the bytes read so far, while live. */
static void
trees_blob_read_blob_pad_0(void *frame_)
{
	trees_blob_read_blob_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, 1, 1);
}

/* trees_blob_read_blob returns the bytes of a blob tree. */
bool
trees_blob_read_blob(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, StdVec *result,
					 ChunksErrorError *error)
{
	trees_blob_read_blob_frame_0_t frame = {.io = io, .store = store, .tree = trees_store_name(tree),
	.result = result, .error = error};

	std_landing(&frame, trees_blob_read_blob_body_0, trees_blob_read_blob_pad_0);
	return frame.ok;
}

/*
 * trees_blob_read_slice fills out with a blob tree's bytes from an offset,
 * reading only the chunks that hold them; a blob that ends before the slice
 * does is data corruption.
 */
bool
trees_blob_read_slice(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint64 offset, uint8 *out,
					  size_t out_len, ChunksErrorError *error)
{
	TreesStoreTree named = trees_store_name(tree);
	size_t		written;
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" ends before the slice read of it")};
	StdFmtArg	args[] = {{&named.address, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	message;

	if (!trees_blob_slice_into(io, store, named, offset, out, out_len, &written, error))
		return false;
	if (written == out_len)
		return true;
	std_fmt_format_Checked(&message, &arguments);
	*error = chunks_error_Error_corrupt(&message);
	return false;
}

/* trees_blob_slice_into_frame_0_t is slice_into's state across its pad: its arguments and the node read, while live. */
typedef struct trees_blob_slice_into_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreTree tree;
	uint64		offset;
	uint8	   *out;
	size_t		out_len;
	size_t	   *result;
	ChunksErrorError *error;
	bool		ok;
	TreesNodeFlat flat;
	bool		flat_live;
} trees_blob_slice_into_frame_0_t;

/* trees_blob_slice_into_pad_0 is slice_into's pad: the node read, while live. */
static void
trees_blob_slice_into_pad_0(void *frame_)
{
	trees_blob_slice_into_frame_0_t *frame = frame_;

	if (frame->flat_live)
		trees_node_Flat_drop(&frame->flat);
}

/*
 * trees_blob_slice_into_body_0 is slice_into's body: a leaf's bytes from the
 * offset copied, or each child's that ends past the offset, until out is
 * full.
 */
static void
trees_blob_slice_into_body_0(void *frame_)
{
	trees_blob_slice_into_frame_0_t *frame = frame_;
	TreesNodeNode node;
	StdBytes	data;
	size_t		written = 0;

	if (!frame->store.ops->read(frame->store.self, frame->io, &frame->tree.address, frame->tree.locator,
								&frame->flat, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->flat_live = true;
	node = trees_node_Flat_node(&frame->flat);
	data = trees_node_Node_blob_data(&node);
	if (data.ptr != NULL)
	{
		size_t		from = std_min_usize(std_usize_try_from_u64(frame->offset), data.len);
		size_t		n = std_min_usize(STD_SUB(usize, data.len, from, 186, 23), frame->out_len);
		size_t		to;

		std_range_to_check(n, frame->out_len, RUST_FILE, 187, 12);
		to = STD_ADD(usize, from, n, 187, 52);
		std_range_check(from, to, data.len, RUST_FILE, 187, 45);
		std_slice_copy_from_slice_Checked(frame->out, n, STD_PTR_AT(data.ptr, from, 1), std_wrapping_sub_usize(to, from),
										  1, 1, RUST_FILE, 187, 24);
		*frame->result = n;
		frame->flat_live = false;
		trees_node_Flat_drop(&frame->flat);
		frame->ok = true;
		return;
	}
	if (trees_node_Node_is_leaf(&node))
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" is not a blob tree")};
		StdFmtArg	args[] = {{&frame->tree.address, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		frame->flat_live = false;
		trees_node_Flat_drop(&frame->flat);
		frame->ok = false;
		return;
	}
	for (size_t i = 0, end = trees_node_Node_len(&node); i < end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		uint64		before;
		uint64		subtree;
		TreesStoreTree child;
		uint64		offset;
		size_t		n;

		if (written == frame->out_len)
			break;
		before = trees_node_Node_before(&node, i);
		subtree = trees_node_Node_subtree(&node, i);
		if (STD_ADD(u64, before, subtree, 198, 12) <= frame->offset)
			continue;
		child = trees_node_Node_child_tree(&node, i);
		offset = std_saturating_sub_u64(frame->offset, trees_node_Node_before(&node, i));
		std_range_from_check(written, frame->out_len, RUST_FILE, 202, 102);
		if (!trees_blob_slice_into(frame->io, frame->store, child, offset, STD_PTR_AT(frame->out, written, 1),
								   std_wrapping_sub_usize(frame->out_len, written), &n, frame->error))
		{
			frame->flat_live = false;
			trees_node_Flat_drop(&frame->flat);
			frame->ok = false;
			return;
		}
		written = STD_ADD(usize, written, n, 201, 9);
	}
	*frame->result = written;
	frame->flat_live = false;
	trees_node_Flat_drop(&frame->flat);
	frame->ok = true;
}

/*
 * trees_blob_slice_into writes a blob tree's bytes from an offset into the
 * start of out, as many as fit, skipping the subtrees before the offset, and
 * returns how many it wrote.
 */
static bool
trees_blob_slice_into(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree, uint64 offset, uint8 *out,
					  size_t out_len, size_t *result, ChunksErrorError *error)
{
	trees_blob_slice_into_frame_0_t frame = {.io = io, .store = store, .tree = tree, .offset = offset, .out = out,
	.out_len = out_len, .result = result, .error = error};

	std_landing(&frame, trees_blob_slice_into_body_0, trees_blob_slice_into_pad_0);
	return frame.ok;
}

/* trees_blob_read_into_frame_0_t is read_into's state across its pad: its arguments and the node read, while live. */
typedef struct trees_blob_read_into_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreTree tree;
	StdVec	   *out;
	ChunksErrorError *error;
	bool		ok;
	TreesNodeFlat flat;
	bool		flat_live;
} trees_blob_read_into_frame_0_t;

/* trees_blob_read_into_pad_0 is read_into's pad: the node read, while live. */
static void
trees_blob_read_into_pad_0(void *frame_)
{
	trees_blob_read_into_frame_0_t *frame = frame_;

	if (frame->flat_live)
		trees_node_Flat_drop(&frame->flat);
}

/* trees_blob_read_into_body_0 is read_into's body: a leaf's bytes appended, or each child's in turn. */
static void
trees_blob_read_into_body_0(void *frame_)
{
	trees_blob_read_into_frame_0_t *frame = frame_;
	TreesNodeNode node;
	StdBytes	data;

	if (!frame->store.ops->read(frame->store.self, frame->io, &frame->tree.address, frame->tree.locator,
								&frame->flat, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->flat_live = true;
	node = trees_node_Flat_node(&frame->flat);
	data = trees_node_Node_blob_data(&node);
	if (data.ptr != NULL)
	{
		std_vec_extend_from_slice_Checked(frame->out, data.ptr, data.len, 1, 1);
		frame->flat_live = false;
		trees_node_Flat_drop(&frame->flat);
		frame->ok = true;
		return;
	}
	if (trees_node_Node_is_leaf(&node))
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" is not a blob tree")};
		StdFmtArg	args[] = {{&frame->tree.address, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		frame->flat_live = false;
		trees_node_Flat_drop(&frame->flat);
		frame->ok = false;
		return;
	}
	std_vec_reserve_Checked(frame->out, (size_t) trees_node_Node_total(&node), 1, 1);
	for (size_t i = 0, end = trees_node_Node_len(&node); i < end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		TreesStoreTree child = trees_node_Node_child_tree(&node, i);

		if (!trees_blob_read_into(frame->io, frame->store, child, frame->out, frame->error))
		{
			frame->flat_live = false;
			trees_node_Flat_drop(&frame->flat);
			frame->ok = false;
			return;
		}
	}
	frame->flat_live = false;
	trees_node_Flat_drop(&frame->flat);
	frame->ok = true;
}

/* trees_blob_read_into appends the bytes of a blob tree. */
static bool
trees_blob_read_into(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree, StdVec *out,
					 ChunksErrorError *error)
{
	trees_blob_read_into_frame_0_t frame = {.io = io, .store = store, .tree = tree, .out = out, .error = error};

	std_landing(&frame, trees_blob_read_into_body_0, trees_blob_read_into_pad_0);
	return frame.ok;
}
