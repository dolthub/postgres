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
 * The C port of trees' build.rs: building trees bottom-up from entries in
 * key order, which gives the one tree that those entries make.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/header.h"
#include "doltgres/chunks/lib.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/node.h"

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "build.rs"

/* trees_build_Entry_Clone_clone_frame_0_t is Entry's Clone's state across its pad: how many of the result's fields it cloned. */
typedef struct trees_build_Entry_Clone_clone_frame_0_t
{
	const TreesBuildEntry *self;
	TreesBuildEntry *result;
	int			cloned;
} trees_build_Entry_Clone_clone_frame_0_t;

/* trees_build_Entry_Clone_clone_body_0 is Entry's Clone's body: each field's clone, in order. */
static void
trees_build_Entry_Clone_clone_body_0(void *frame_)
{
	trees_build_Entry_Clone_clone_frame_0_t *frame = frame_;

	std_vec_clone(&frame->result->key, &frame->self->key, 1, 1);
	frame->cloned = 1;
	std_vec_clone(&frame->result->value, &frame->self->value, 1, 1);
	frame->cloned = 2;
	std_vec_clone(&frame->result->addresses, &frame->self->addresses, sizeof(chunks_TupleU32Locator_t),
				  _Alignof(chunks_TupleU32Locator_t));
	frame->cloned = 3;
	std_vec_clone(&frame->result->stamps, &frame->self->stamps, sizeof(ChunksTrailerStamp),
				  _Alignof(ChunksTrailerStamp));
	frame->cloned = 4;
	std_vec_clone(&frame->result->values, &frame->self->values, sizeof(uint32), _Alignof(uint32));
	frame->cloned = 5;
}

/* trees_build_Entry_Clone_clone_pad_0 is Entry's Clone's pad: the fields it cloned, in reverse. */
static void
trees_build_Entry_Clone_clone_pad_0(void *frame_)
{
	trees_build_Entry_Clone_clone_frame_0_t *frame = frame_;

	if (frame->cloned >= 4)
		std_vec_free_Checked(&frame->result->stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	if (frame->cloned >= 3)
		std_vec_free_Checked(&frame->result->addresses, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
	if (frame->cloned >= 2)
		std_vec_free_Checked(&frame->result->value, 1, 1);
	if (frame->cloned >= 1)
		std_vec_free_Checked(&frame->result->key, 1, 1);
}

/* trees_build_Entry_Clone_clone is Entry's derived Clone: each field's, in order, the ones cloned dropped should a later one unwind. */
void
trees_build_Entry_Clone_clone(const TreesBuildEntry *self, TreesBuildEntry *result)
{
	trees_build_Entry_Clone_clone_frame_0_t frame = {.self = self, .result = result};

	std_landing(&frame, trees_build_Entry_Clone_clone_body_0, trees_build_Entry_Clone_clone_pad_0);
}

/* trees_build_Entry_Debug_fmt is Entry's derived Debug. */
bool
trees_build_Entry_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesBuildEntry *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Entry", 5);

	std_DebugStruct_field(&s, "key", 3, &self->key, chunks_chunk_VecU8_Debug_fmt);
	std_DebugStruct_field(&s, "value", 5, &self->value, chunks_chunk_VecU8_Debug_fmt);
	std_DebugStruct_field(&s, "addresses", 9, &self->addresses, trees_node_VecTupleU32Locator_Debug_fmt);
	std_DebugStruct_field(&s, "stamps", 6, &self->stamps, chunks_trailer_VecStamp_Debug_fmt);
	std_DebugStruct_field(&s, "values", 6, &self->values, chunks_header_VecU32_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* trees_build_SliceU32_PartialEqSliceU32_eq is [u32]'s PartialEq: the same length and values. */
static bool
trees_build_SliceU32_PartialEqSliceU32_eq(const uint32 *self, size_t self_len, const uint32 *other, size_t other_len)
{
	if (self_len != other_len)
		return false;
	for (size_t k = 0, k_len = self_len; k < k_len; k++)
		if (self[k] != other[k])
			return false;
	return true;
}

/* trees_build_VecU32_PartialEqVecU32_eq is Vec<u32>'s PartialEq, its slices'. */
static bool
trees_build_VecU32_PartialEqVecU32_eq(const StdVec *self, const StdVec *other)
{
	return trees_build_SliceU32_PartialEqSliceU32_eq(self->ptr, self->len, other->ptr, other->len);
}

/* trees_build_Entry_PartialEq_eq is Entry's derived PartialEq: every field equal, in order. */
bool
trees_build_Entry_PartialEq_eq(const TreesBuildEntry *self, const TreesBuildEntry *other)
{
	return trees_store_VecU8_PartialEq_eq(&self->key, &other->key) &&
		trees_store_VecU8_PartialEq_eq(&self->value, &other->value) &&
		trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq(&self->addresses, &other->addresses) &&
		chunks_trailer_VecStamp_PartialEq_eq(&self->stamps, &other->stamps) &&
		trees_build_VecU32_PartialEqVecU32_eq(&self->values, &other->values);
}

/* trees_build_Entry_drop is Entry's drop glue: its five vectors, in order. */
void
trees_build_Entry_drop(TreesBuildEntry *self)
{
	std_vec_free_Checked(&self->key, 1, 1);
	std_vec_free_Checked(&self->value, 1, 1);
	std_vec_free_Checked(&self->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_free_Checked(&self->stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_free_Checked(&self->values, sizeof(uint32), _Alignof(uint32));
}

/* trees_build_FromFn_drop is FromFn's drop glue: its function's captures'. */
void
trees_build_FromFn_drop(TreesBuildFromFn *self)
{
	if (self->f_drop != NULL)
		self->f_drop(self->f_captures);
}

/* trees_build_from_fn is from_fn: the entries a function gives, one for each call, until it returns None; it takes f. */
TreesBuildFromFn
trees_build_from_fn(bool (*f) (void *captures, HostIoIo *io, bool *some, TreesBuildEntry *result,
							   ChunksErrorError *error),
					void *f_captures, void (*f_drop) (void *))
{
	TreesBuildFromFn result = {f, f_captures, f_drop};

	return result;
}

/* trees_build_FromFn_Entries_next_entry is FromFn's next_entry: the function's call with the caller's Io. */
bool
trees_build_FromFn_Entries_next_entry(void *self_, HostIoIo *io, bool *some, TreesBuildEntry *result,
									  ChunksErrorError *error)
{
	TreesBuildFromFn *self = self_;

	return self->f(self->f_captures, io, some, result, error);
}

/* TREES_BUILD_FROM_FN_ENTRIES_OPS is FromFn's Entries table. */
const TreesBuildEntriesOps TREES_BUILD_FROM_FN_ENTRIES_OPS = {
	.next_entry = trees_build_FromFn_Entries_next_entry,
};

#ifdef DOLTGRES_TEST_TREES

/* trees_build_Entry_new_VecU8Array8U8_frame_0_t is Entry::new's state across its pad, at <Vec<u8>, [u8; 8]>: the key it takes, while live. */
typedef struct trees_build_Entry_new_VecU8Array8U8_frame_0_t
{
	StdVec	   *key;
	bool		key_live;
	const uint8 *value;
	TreesBuildEntry *result;
} trees_build_Entry_new_VecU8Array8U8_frame_0_t;

/* trees_build_Entry_new_VecU8Array8U8_body_0 is Entry::new's body at <Vec<u8>, [u8; 8]>: the key's Into, the array's into a new Vec, and no addresses or stamps. */
static void
trees_build_Entry_new_VecU8Array8U8_body_0(void *frame_)
{
	trees_build_Entry_new_VecU8Array8U8_frame_0_t *frame = frame_;
	StdVec		value;
	uint8	   *boxed = std_box_new_uninit_layout_Checked(8, 1);

	memcpy(boxed, frame->value, 8);
	std_box_slice_into_vec_Checked(&value, boxed, 8, 1);
	frame->key_live = false;
	frame->result->key = *frame->key;
	frame->result->value = value;
	std_vec_new(&frame->result->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_new(&frame->result->stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_new(&frame->result->values, sizeof(uint32), _Alignof(uint32));
}

/* trees_build_Entry_new_VecU8Array8U8_pad_0 is Entry::new's pad at <Vec<u8>, [u8; 8]>: the key, while live. */
static void
trees_build_Entry_new_VecU8Array8U8_pad_0(void *frame_)
{
	trees_build_Entry_new_VecU8Array8U8_frame_0_t *frame = frame_;

	if (frame->key_live)
		std_vec_free_Checked(frame->key, 1, 1);
}

/*
 * trees_build_Entry_new_VecU8Array8U8 is Entry::new at <Vec<u8>, [u8; 8]>:
 * an entry of a key and an eight-byte value with no addresses or stamps; it
 * takes both.
 */
void
trees_build_Entry_new_VecU8Array8U8(StdVec *key, const uint8 *value, TreesBuildEntry *result)
{
	trees_build_Entry_new_VecU8Array8U8_frame_0_t frame = {.key = key, .key_live = true, .value = value,
	.result = result};

	std_landing(&frame, trees_build_Entry_new_VecU8Array8U8_body_0, trees_build_Entry_new_VecU8Array8U8_pad_0);
}

#endif							/* DOLTGRES_TEST_TREES */

/*
 * trees_build_Entry_new_VecU8VecU8 is Entry::new at <Vec<u8>, Vec<u8>>: an
 * entry of a key and a value, each its own Into, with no addresses or
 * stamps; it takes both.
 */
void
trees_build_Entry_new_VecU8VecU8(StdVec *key, StdVec *value, TreesBuildEntry *result)
{
	result->key = *key;
	result->value = *value;
	std_vec_new(&result->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_new(&result->stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_new(&result->values, sizeof(uint32), _Alignof(uint32));
}

/* trees_build_Item_Clone_clone_frame_0_t is Item's Clone's state across its pad: whether a child's key is cloned. */
typedef struct trees_build_Item_Clone_clone_frame_0_t
{
	const TreesBuildItem *self;
	TreesBuildItem *result;
	bool		key_live;
} trees_build_Item_Clone_clone_frame_0_t;

/* trees_build_Item_Clone_clone_body_0 is Item's Clone's body: the variant's fields cloned, an Arc's count raised. */
static void
trees_build_Item_Clone_clone_body_0(void *frame_)
{
	trees_build_Item_Clone_clone_frame_0_t *frame = frame_;
	const TreesBuildItem *self = frame->self;
	TreesBuildItem *result = frame->result;

	switch (self->tag)
	{
		case TREES_BUILD_ITEM_LEAF:
			trees_build_Entry_Clone_clone(&self->u.leaf, &result->u.leaf);
			break;
		case TREES_BUILD_ITEM_OLD:
			result->u.old.node.ptr = std_arc_clone(self->u.old.node.ptr);
			result->u.old.node.len = self->u.old.node.len;
			result->u.old.index = self->u.old.index;
			result->u.old.stamps = std_arc_clone(self->u.old.stamps);
			break;
		case TREES_BUILD_ITEM_CHILD:
			std_vec_clone(&result->u.child.key, &self->u.child.key, 1, 1);
			frame->key_live = true;
			result->u.child.tree = self->u.child.tree;
			result->u.child.count = self->u.child.count;
			result->u.child.max_tuple = self->u.child.max_tuple;
			result->u.child.has_summary = self->u.child.has_summary;
			if (self->u.child.has_summary)
				std_vec_clone(&result->u.child.summary, &self->u.child.summary, 1, 1);
			frame->key_live = false;
			break;
	}
	result->tag = self->tag;
}

/* trees_build_Item_Clone_clone_pad_0 is Item's Clone's pad: a child's cloned key, while live. */
static void
trees_build_Item_Clone_clone_pad_0(void *frame_)
{
	trees_build_Item_Clone_clone_frame_0_t *frame = frame_;

	if (frame->key_live)
		std_vec_free_Checked(&frame->result->u.child.key, 1, 1);
}

/* trees_build_Item_Clone_clone is Item's derived Clone: the variant's fields cloned in order, a child's key dropped should its summary's clone unwind. */
void
trees_build_Item_Clone_clone(const TreesBuildItem *self, TreesBuildItem *result)
{
	trees_build_Item_Clone_clone_frame_0_t frame = {.self = self, .result = result};

	std_landing(&frame, trees_build_Item_Clone_clone_body_0, trees_build_Item_Clone_clone_pad_0);
}

/* trees_build_ArcSliceU8_Debug_fmt is Arc<[u8]>'s Debug, its slice's. */
static bool
trees_build_ArcSliceU8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdArcSlice *self = value;
	StdBytes	bytes = {STD_ARC_VALUE(self->ptr, 1), self->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* trees_build_ArcStamps_Debug_fmt is Arc<Stamps>'s Debug, its stamps'. */
static bool
trees_build_ArcStamps_Debug_fmt(const void *value, StdFormatter *f)
{
	StdArcInner *const *self = value;

	return chunks_trailer_Stamps_Debug_fmt(std_arc_Deref_deref(*self, _Alignof(ChunksTrailerStamps)), f);
}

/* trees_build_Item_Debug_fmt is Item's derived Debug. */
bool
trees_build_Item_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesBuildItem *self = value;

	switch (self->tag)
	{
		case TREES_BUILD_ITEM_LEAF:
			{
				StdDebugTuple t = std_fmt_debug_tuple(f, "Leaf", 4);

				std_DebugTuple_field(&t, &self->u.leaf, trees_build_Entry_Debug_fmt);
				return std_DebugTuple_finish(&t);
			}
		case TREES_BUILD_ITEM_OLD:
			{
				StdDebugStruct s = std_fmt_debug_struct(f, "Old", 3);

				std_DebugStruct_field(&s, "node", 4, &self->u.old.node, trees_build_ArcSliceU8_Debug_fmt);
				std_DebugStruct_field(&s, "index", 5, &self->u.old.index, std_usize_Debug_fmt);
				std_DebugStruct_field(&s, "stamps", 6, &self->u.old.stamps, trees_build_ArcStamps_Debug_fmt);
				return std_DebugStruct_finish(&s);
			}
		case TREES_BUILD_ITEM_CHILD:
			{
				StdDebugStruct s = std_fmt_debug_struct(f, "Child", 5);
				StdOptionRef summary = {self->u.child.has_summary,
				{&self->u.child.summary, chunks_chunk_VecU8_Debug_fmt}};

				std_DebugStruct_field(&s, "key", 3, &self->u.child.key, chunks_chunk_VecU8_Debug_fmt);
				std_DebugStruct_field(&s, "tree", 4, &self->u.child.tree, trees_store_Tree_Debug_fmt);
				std_DebugStruct_field(&s, "count", 5, &self->u.child.count, std_u64_Debug_fmt);
				std_DebugStruct_field(&s, "max_tuple", 9, &self->u.child.max_tuple, std_u64_Debug_fmt);
				std_DebugStruct_field(&s, "summary", 7, &summary, std_option_Debug_fmt);
				return std_DebugStruct_finish(&s);
			}
	}
	pg_unreachable();
}

/* trees_build_Item_drop is Item's drop glue: the variant's fields', an Arc's count lowered. */
void
trees_build_Item_drop(TreesBuildItem *self)
{
	switch (self->tag)
	{
		case TREES_BUILD_ITEM_LEAF:
			trees_build_Entry_drop(&self->u.leaf);
			break;
		case TREES_BUILD_ITEM_OLD:
			trees_cursor_ArcSliceU8_drop(&self->u.old.node);
			trees_cursor_ArcStamps_drop(self->u.old.stamps);
			break;
		case TREES_BUILD_ITEM_CHILD:
			std_vec_free_Checked(&self->u.child.key, 1, 1);
			if (self->u.child.has_summary)
				std_vec_free_Checked(&self->u.child.summary, 1, 1);
			break;
	}
}

/* trees_build_VecItem_drop is Vec<Item>'s drop glue: each item's, then the buffer. */
void
trees_build_VecItem_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		trees_build_Item_drop(&STD_VEC_AT(self, TreesBuildItem, k));
	std_vec_free_Checked(self, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
}

/* trees_build_Item_key is Item::key: the item's key. */
StdBytes
trees_build_Item_key(const TreesBuildItem *self)
{
	StdBytes	key = {0};

	switch (self->tag)
	{
		case TREES_BUILD_ITEM_LEAF:
			key.ptr = self->u.leaf.key.ptr;
			key.len = self->u.leaf.key.len;
			break;
		case TREES_BUILD_ITEM_OLD:
			{
				TreesNodeNode node = trees_node_Node_view(STD_ARC_VALUE(self->u.old.node.ptr, 1), self->u.old.node.len);

				key = trees_node_Node_key(&node, self->u.old.index);
			}
			break;
		case TREES_BUILD_ITEM_CHILD:
			key.ptr = self->u.child.key.ptr;
			key.len = self->u.child.key.len;
			break;
	}
	return key;
}

/* trees_build_Item_size is Item::size: the size the item counts for in its node. */
uint64
trees_build_Item_size(const TreesBuildItem *self)
{
	switch (self->tag)
	{
		case TREES_BUILD_ITEM_LEAF:
			return trees_chunker_leaf_entry_size(self->u.leaf.key.ptr, self->u.leaf.key.len, self->u.leaf.value.ptr,
												 self->u.leaf.value.len);
		case TREES_BUILD_ITEM_OLD:
			{
				TreesNodeNode node = trees_node_Node_view(STD_ARC_VALUE(self->u.old.node.ptr, 1), self->u.old.node.len);
				StdBytes	key = trees_node_Node_key(&node, self->u.old.index);
				StdBytes	value = trees_node_Node_value(&node, self->u.old.index);

				return trees_chunker_leaf_entry_size(key.ptr, key.len, value.ptr, value.len);
			}
		case TREES_BUILD_ITEM_CHILD:
			return trees_chunker_internal_entry_size(self->u.child.key.ptr, self->u.child.key.len);
	}
	pg_unreachable();
}

/* trees_build_Item_weight is Item::weight: how many leaf entries the item stands for, one for an entry and a child's subtree's count. */
uint64
trees_build_Item_weight(const TreesBuildItem *self)
{
	switch (self->tag)
	{
		case TREES_BUILD_ITEM_LEAF:
		case TREES_BUILD_ITEM_OLD:
			return 1;
		case TREES_BUILD_ITEM_CHILD:
			return self->u.child.count;
	}
	pg_unreachable();
}

/* trees_build_Item_check is Item::check: an error for an item too large for a node. */
bool
trees_build_Item_check(const TreesBuildItem *self, ChunksErrorError *error)
{
	if ((size_t) trees_build_Item_size(self) > CHUNKS_MAX_ENTRY)
	{
		uint64		size = trees_build_Item_size(self);
		size_t		max_entry = CHUNKS_MAX_ENTRY;
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("tree entry of "), STD_FMT_ARG(0),
		STD_FMT_LIT(" bytes is larger than "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&size, std_u64_Display_fmt}, {&max_entry, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	return true;
}

/* trees_build_VecVecU32_drop is Vec<Vec<u32>>'s drop glue: each vector's, then the buffer. */
static void
trees_build_VecVecU32_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(self, StdVec, k), sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(self, sizeof(StdVec), _Alignof(StdVec));
}

/* trees_build_write_node_frame_0_t is write_node's state across its pad: its arguments, and the vectors, stamps, and summary it holds, while live. */
typedef struct trees_build_write_node_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	uint8		level;
	const TreesBuildItem *items;
	size_t		items_len;
	const TreesShape *shape;
	TreesBuildItem *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		keys;
	bool		keys_live;
	StdVec		locators;
	bool		locators_live;
	StdVec		values;
	bool		values_live;
	StdVec		addresses;
	bool		addresses_live;
	StdVec		inner;
	bool		inner_live;
	StdVec		children;
	bool		children_live;
	StdVec		counts;
	bool		counts_live;
	StdVec		summaries;
	bool		summaries_live;
	StdVec		max_tuples;
	bool		max_tuples_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	bool		has_summary;
	StdVec		summary;
	bool		summary_live;
	StdVec		hashed;
	bool		hashed_live;
} trees_build_write_node_frame_0_t;

/* trees_build_write_node_body_0 is write_node's body. */
static void
trees_build_write_node_body_0(void *frame_)
{
	trees_build_write_node_frame_0_t *frame = frame_;
	const TreesShape *shape = frame->shape;
	bool		comparator = !trees_KeyOrder_is_bytes(&shape->order);
	size_t		items_len = frame->items_len;
	uint64		count;
	uint64		max_tuple;
	TreesStoreTreeRef written;

	{
		StdSizeHint hint = {items_len, true, items_len};

		std_vec_with_capacity_trusted(&frame->keys, hint, sizeof(StdBytes), _Alignof(StdBytes));
		frame->keys_live = true;
		std_vec_extend_trusted_reserve(&frame->keys, hint, sizeof(StdBytes), _Alignof(StdBytes));
		for (size_t k = 0, k_len = items_len; k < k_len; k++)
		{
			STD_VEC_AT(&frame->keys, StdBytes, frame->keys.len) = trees_build_Item_key(&frame->items[k]);
			frame->keys.len = std_wrapping_add_usize(frame->keys.len, 1);
		}
	}
	std_vec_new(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	frame->locators_live = true;
	if (frame->level == 0)
	{
		std_vec_with_capacity_Checked(&frame->values, items_len, sizeof(StdBytes), _Alignof(StdBytes));
		frame->values_live = true;
		std_vec_with_capacity_Checked(&frame->addresses, items_len, sizeof(StdVec), _Alignof(StdVec));
		frame->addresses_live = true;
		frame->stamps = chunks_trailer_Stamps_Default_default();
		frame->stamps_live = true;
		for (size_t k = 0, k_len = items_len; k < k_len; k++)
		{
			const TreesBuildItem *item = &frame->items[k];
			StdBytes	value;
			StdVec		offsets;

			switch (item->tag)
			{
				case TREES_BUILD_ITEM_LEAF:
					if (item->u.leaf.stamps.len != 0)
					{
						chunks_trailer_Stamps_push(&frame->stamps, item->u.leaf.stamps.ptr, item->u.leaf.stamps.len);
						chunks_trailer_Stamps_push_values(&frame->stamps, item->u.leaf.values.ptr,
														  item->u.leaf.values.len);
					}
					value.ptr = item->u.leaf.value.ptr;
					value.len = item->u.leaf.value.len;
					std_vec_clone(&frame->inner, &item->u.leaf.addresses, sizeof(chunks_TupleU32Locator_t),
								  _Alignof(chunks_TupleU32Locator_t));
					frame->inner_live = true;
					break;
				case TREES_BUILD_ITEM_OLD:
					{
						const ChunksTrailerStamps *old = std_arc_Deref_deref(item->u.old.stamps,
																			 _Alignof(ChunksTrailerStamps));
						TreesNodeNode node;

						if (chunks_trailer_Stamps_entry_len(old, item->u.old.index) > 0)
							chunks_trailer_Stamps_push_entry_of(&frame->stamps, old, item->u.old.index);
						node = trees_node_Node_view(STD_ARC_VALUE(item->u.old.node.ptr, 1), item->u.old.node.len);
						value = trees_node_Node_value(&node, item->u.old.index);
						trees_node_Node_value_addresses(&node, item->u.old.index, &frame->inner);
						frame->inner_live = true;
					}
					break;
				case TREES_BUILD_ITEM_CHILD:
					{
						static const char text[] = "internal error: entered unreachable code: a leaf holds only leaf entries";

						std_panic(RUST_FILE, 154, 39, text, sizeof(text) - 1);
					}
			}
			std_vec_push_Checked(&frame->values, &value, sizeof(StdBytes), _Alignof(StdBytes));
			{
				StdSizeHint hint = {frame->inner.len, true, frame->inner.len};

				std_vec_with_capacity_trusted(&offsets, hint, sizeof(uint32), _Alignof(uint32));
				std_vec_extend_trusted_reserve(&offsets, hint, sizeof(uint32), _Alignof(uint32));
				for (size_t j = 0, j_len = frame->inner.len; j < j_len; j++)
				{
					STD_VEC_AT(&offsets, uint32, offsets.len) =
						STD_VEC_AT(&frame->inner, chunks_TupleU32Locator_t, j).f0;
					offsets.len = std_wrapping_add_usize(offsets.len, 1);
				}
			}
			std_vec_push_Checked(&frame->addresses, &offsets, sizeof(StdVec), _Alignof(StdVec));
			{
				StdSizeHint hint = {frame->inner.len, true, frame->inner.len};

				std_vec_extend_trusted_reserve(&frame->locators, hint, sizeof(ChunksTrailerLocator),
											   _Alignof(ChunksTrailerLocator));
				for (size_t j = 0, j_len = frame->inner.len; j < j_len; j++)
				{
					STD_VEC_AT(&frame->locators, ChunksTrailerLocator, frame->locators.len) =
						STD_VEC_AT(&frame->inner, chunks_TupleU32Locator_t, j).f1;
					frame->locators.len = std_wrapping_add_usize(frame->locators.len, 1);
				}
			}
			frame->inner_live = false;
			std_vec_free_Checked(&frame->inner, sizeof(chunks_TupleU32Locator_t),
								 _Alignof(chunks_TupleU32Locator_t));
		}
		if (chunks_trailer_Stamps_entries(&frame->stamps) != 0 &&
			chunks_trailer_Stamps_entries(&frame->stamps) != items_len)
		{
			static const char text[] = "a leaf's entries must all have stamps or none";

			*frame->error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
			frame->stamps_live = false;
			chunks_trailer_Stamps_drop(&frame->stamps);
			frame->addresses_live = false;
			trees_build_VecVecU32_drop(&frame->addresses);
			frame->values_live = false;
			std_vec_free_Checked(&frame->values, sizeof(StdBytes), _Alignof(StdBytes));
			frame->locators_live = false;
			std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
			frame->keys_live = false;
			std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
			frame->ok = false;
			return;
		}
		frame->has_summary = shape->has_summarizer;
		if (shape->has_summarizer)
		{
			if (!trees_Summarizer_summarize(&shape->summarizer, frame->io, frame->level, frame->values.ptr,
											frame->values.len, &frame->summary, frame->error))
			{
				frame->stamps_live = false;
				chunks_trailer_Stamps_drop(&frame->stamps);
				frame->addresses_live = false;
				trees_build_VecVecU32_drop(&frame->addresses);
				frame->values_live = false;
				std_vec_free_Checked(&frame->values, sizeof(StdBytes), _Alignof(StdBytes));
				frame->locators_live = false;
				std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator),
									 _Alignof(ChunksTrailerLocator));
				frame->keys_live = false;
				std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
				frame->ok = false;
				return;
			}
			frame->summary_live = true;
		}
		trees_node_encode_leaf(frame->keys.ptr, frame->keys.len, frame->values.ptr, frame->values.len,
							   frame->addresses.ptr, frame->addresses.len, comparator, &frame->hashed);
		frame->hashed_live = true;
		count = (uint64) items_len;
		frame->addresses_live = false;
		trees_build_VecVecU32_drop(&frame->addresses);
		frame->values_live = false;
		std_vec_free_Checked(&frame->values, sizeof(StdBytes), _Alignof(StdBytes));
	}
	else
	{
		uint64		total = 0;

		std_vec_with_capacity_Checked(&frame->children, items_len, sizeof(ChunksAddressAddress),
									  _Alignof(ChunksAddressAddress));
		frame->children_live = true;
		std_vec_with_capacity_Checked(&frame->counts, items_len, sizeof(uint64), _Alignof(uint64));
		frame->counts_live = true;
		std_vec_with_capacity_Checked(&frame->summaries, items_len, sizeof(StdBytes), _Alignof(StdBytes));
		frame->summaries_live = true;
		std_vec_with_capacity_Checked(&frame->max_tuples, items_len, sizeof(uint64), _Alignof(uint64));
		frame->max_tuples_live = true;
		for (size_t k = 0, k_len = items_len; k < k_len; k++)
		{
			const TreesBuildItem *item = &frame->items[k];

			if (item->tag != TREES_BUILD_ITEM_CHILD)
			{
				static const char text[] = "internal error: entered unreachable code: an internal node holds only internal entries";

				std_panic(RUST_FILE, 175, 17, text, sizeof(text) - 1);
			}
			std_vec_push_Checked(&frame->children, &item->u.child.tree.address, sizeof(ChunksAddressAddress),
								 _Alignof(ChunksAddressAddress));
			std_vec_push_Checked(&frame->locators, &item->u.child.tree.locator, sizeof(ChunksTrailerLocator),
								 _Alignof(ChunksTrailerLocator));
			std_vec_push_Checked(&frame->counts, &item->u.child.count, sizeof(uint64), _Alignof(uint64));
			std_vec_push_Checked(&frame->max_tuples, &item->u.child.max_tuple, sizeof(uint64), _Alignof(uint64));
			if (shape->has_summarizer)
			{
				StdBytes	summary;

				if (!item->u.child.has_summary)
				{
					static const char text[] = "a summarized tree's child has no summary";

					*frame->error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
					frame->max_tuples_live = false;
					std_vec_free_Checked(&frame->max_tuples, sizeof(uint64), _Alignof(uint64));
					frame->summaries_live = false;
					std_vec_free_Checked(&frame->summaries, sizeof(StdBytes), _Alignof(StdBytes));
					frame->counts_live = false;
					std_vec_free_Checked(&frame->counts, sizeof(uint64), _Alignof(uint64));
					frame->children_live = false;
					std_vec_free_Checked(&frame->children, sizeof(ChunksAddressAddress),
										 _Alignof(ChunksAddressAddress));
					frame->locators_live = false;
					std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator),
										 _Alignof(ChunksTrailerLocator));
					frame->keys_live = false;
					std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
					frame->ok = false;
					return;
				}
				summary.ptr = item->u.child.summary.ptr;
				summary.len = item->u.child.summary.len;
				std_vec_push_Checked(&frame->summaries, &summary, sizeof(StdBytes), _Alignof(StdBytes));
			}
		}
		for (size_t k = 0, k_len = frame->counts.len; k < k_len; k++)
			total = std_add_u64(total, STD_VEC_AT(&frame->counts, uint64, k), STD_ACCUM_FILE, 206, 1);
		frame->has_summary = shape->has_summarizer;
		if (shape->has_summarizer)
		{
			if (!trees_Summarizer_summarize(&shape->summarizer, frame->io, frame->level, frame->summaries.ptr,
											frame->summaries.len, &frame->summary, frame->error))
			{
				frame->max_tuples_live = false;
				std_vec_free_Checked(&frame->max_tuples, sizeof(uint64), _Alignof(uint64));
				frame->summaries_live = false;
				std_vec_free_Checked(&frame->summaries, sizeof(StdBytes), _Alignof(StdBytes));
				frame->counts_live = false;
				std_vec_free_Checked(&frame->counts, sizeof(uint64), _Alignof(uint64));
				frame->children_live = false;
				std_vec_free_Checked(&frame->children, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
				frame->locators_live = false;
				std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator),
									 _Alignof(ChunksTrailerLocator));
				frame->keys_live = false;
				std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
				frame->ok = false;
				return;
			}
			frame->summary_live = true;
		}
		trees_node_encode_internal_with_summaries(frame->level, frame->keys.ptr, frame->keys.len,
												  frame->children.ptr, frame->children.len, frame->counts.ptr,
												  frame->counts.len,
												  shape->has_summarizer ? frame->summaries.ptr : NULL,
												  shape->has_summarizer ? frame->summaries.len : 0, comparator,
												  &frame->hashed);
		frame->hashed_live = true;
		frame->max_tuples_live = false;
		frame->stamps = chunks_trailer_Stamps_subtrees(&frame->max_tuples);
		frame->stamps_live = true;
		count = total;
		frame->summaries_live = false;
		std_vec_free_Checked(&frame->summaries, sizeof(StdBytes), _Alignof(StdBytes));
		frame->counts_live = false;
		std_vec_free_Checked(&frame->counts, sizeof(uint64), _Alignof(uint64));
		frame->children_live = false;
		std_vec_free_Checked(&frame->children, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	}
	max_tuple = chunks_trailer_Stamps_max_tuple(&frame->stamps);
	frame->hashed_live = false;
	if (!frame->store.ops->write(frame->store.self, frame->io, &frame->hashed, frame->locators.ptr,
								 frame->locators.len, &frame->stamps, &written, frame->error))
	{
		if (frame->summary_live)
		{
			frame->summary_live = false;
			std_vec_free_Checked(&frame->summary, 1, 1);
		}
		frame->stamps_live = false;
		chunks_trailer_Stamps_drop(&frame->stamps);
		frame->locators_live = false;
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		frame->keys_live = false;
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
		frame->ok = false;
		return;
	}
	frame->result->tag = TREES_BUILD_ITEM_CHILD;
	if (frame->keys.len == 0)
		std_vec_new(&frame->result->u.child.key, 1, 1);
	else
	{
		const StdBytes *last = &STD_VEC_AT(&frame->keys, StdBytes, std_wrapping_sub_usize(frame->keys.len, 1));

		std_slice_to_vec_Checked(&frame->result->u.child.key, last->ptr, last->len, 1, 1);
	}
	frame->result->u.child.tree = trees_store_TreeRef_tree(&written);
	frame->result->u.child.count = count;
	frame->result->u.child.max_tuple = max_tuple;
	frame->result->u.child.has_summary = frame->has_summary;
	if (frame->has_summary)
	{
		frame->summary_live = false;
		frame->result->u.child.summary = frame->summary;
	}
	frame->stamps_live = false;
	chunks_trailer_Stamps_drop(&frame->stamps);
	frame->locators_live = false;
	std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	frame->keys_live = false;
	std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	frame->ok = true;
}

/* trees_build_write_node_pad_0 is write_node's pad: what its body holds, while live, in reverse order of declaration. */
static void
trees_build_write_node_pad_0(void *frame_)
{
	trees_build_write_node_frame_0_t *frame = frame_;

	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
	if (frame->summary_live)
		std_vec_free_Checked(&frame->summary, 1, 1);
	if (frame->inner_live)
		std_vec_free_Checked(&frame->inner, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	if (frame->max_tuples_live)
		std_vec_free_Checked(&frame->max_tuples, sizeof(uint64), _Alignof(uint64));
	if (frame->summaries_live)
		std_vec_free_Checked(&frame->summaries, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->counts_live)
		std_vec_free_Checked(&frame->counts, sizeof(uint64), _Alignof(uint64));
	if (frame->children_live)
		std_vec_free_Checked(&frame->children, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->stamps_live)
		chunks_trailer_Stamps_drop(&frame->stamps);
	if (frame->addresses_live)
		trees_build_VecVecU32_drop(&frame->addresses);
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
}

/*
 * trees_build_write_node is write_node: a node of the items at the level
 * written, returning the internal item naming it, with the summary of its
 * subtree the shape's summarizer makes from the node's leaf values or its
 * children's summaries.
 */
bool
trees_build_write_node(HostIoIo *io, TreesStoreNodeStore store, uint8 level, const TreesBuildItem *items,
					   size_t items_len, const TreesShape *shape, TreesBuildItem *result, ChunksErrorError *error)
{
	trees_build_write_node_frame_0_t frame = {.io = io, .store = store, .level = level, .items = items,
	.items_len = items_len, .shape = shape, .result = result, .error = error};

	std_landing(&frame, trees_build_write_node_body_0, trees_build_write_node_pad_0);
	return frame.ok;
}

/* trees_build_Level_drop is Level's drop glue: its splitter's, then its items'. */
static void
trees_build_Level_drop(TreesBuildLevel *self)
{
	trees_chunker_Splitter_drop(&self->splitter);
	trees_build_VecItem_drop(&self->items);
}

/* trees_build_Builder_new is Builder::new: a builder of a tree with the target node size, in the default shape. */
void
trees_build_Builder_new(TreesStoreNodeStore store, uint32 target, TreesBuildBuilder *result)
{
	TreesShape	shape = trees_Shape_Default_default();

	trees_build_Builder_shaped(store, target, &shape, result);
	trees_Shape_drop(&shape);
}

/* trees_build_Builder_shaped is Builder::shaped: a builder of a tree of the shape with the target node size. */
void
trees_build_Builder_shaped(TreesStoreNodeStore store, uint32 target, const TreesShape *shape,
						   TreesBuildBuilder *result)
{
	trees_build_Builder_at_level(store, target, 0, shape, result);
}

/* trees_build_Builder_at_level_frame_0_t is at_level's state across its pad: the builder it fills, whose shape and levels are live once made. */
typedef struct trees_build_Builder_at_level_frame_0_t
{
	TreesStoreNodeStore store;
	uint32		target;
	uint8		level;
	const TreesShape *shape;
	TreesBuildBuilder *result;
	bool		shape_live;
	bool		levels_live;
} trees_build_Builder_at_level_frame_0_t;

/* trees_build_Builder_at_level_body_0 is at_level's body: the shape's clone, then a level being built from 0 through the level, a Map<RangeInclusive<u8>>'s collect. */
static void
trees_build_Builder_at_level_body_0(void *frame_)
{
	trees_build_Builder_at_level_frame_0_t *frame = frame_;
	TreesBuildBuilder *result = frame->result;
	size_t		steps = std_wrapping_add_usize((size_t) frame->level, 1);
	StdSizeHint hint = {steps, true, steps};

	result->store = frame->store;
	result->target = frame->target;
	trees_Shape_Clone_clone(frame->shape, &result->shape);
	frame->shape_live = true;
	std_vec_with_capacity_trusted(&result->levels, hint, sizeof(TreesBuildLevel), _Alignof(TreesBuildLevel));
	frame->levels_live = true;
	std_vec_extend_trusted_reserve(&result->levels, hint, sizeof(TreesBuildLevel), _Alignof(TreesBuildLevel));
	{
		uint8		l = 0;
		bool		l_done = false;

		for (; !l_done; l_done = (l == frame->level), l = l_done ? l : std_forward_unchecked_u8_Checked(l, 1))
		{
			TreesBuildLevel built = {.splitter = trees_chunker_Splitter_new(l, frame->target)};

			std_vec_new(&built.items, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
			STD_VEC_AT(&result->levels, TreesBuildLevel, result->levels.len) = built;
			result->levels.len = std_wrapping_add_usize(result->levels.len, 1);
		}
	}
	result->base = (size_t) frame->level;
	frame->levels_live = false;
	frame->shape_live = false;
}

/* trees_build_Builder_at_level_pad_0 is at_level's pad: the levels made so far and the shape's clone, while live. */
static void
trees_build_Builder_at_level_pad_0(void *frame_)
{
	trees_build_Builder_at_level_frame_0_t *frame = frame_;

	if (frame->levels_live)
	{
		for (size_t k = 0, k_len = frame->result->levels.len; k < k_len; k++)
			trees_build_Level_drop(&STD_VEC_AT(&frame->result->levels, TreesBuildLevel, k));
		std_vec_free_Checked(&frame->result->levels, sizeof(TreesBuildLevel), _Alignof(TreesBuildLevel));
	}
	if (frame->shape_live)
		trees_Shape_drop(&frame->result->shape);
}

/* trees_build_Builder_at_level is Builder::at_level: a builder whose first items are at the level. */
void
trees_build_Builder_at_level(TreesStoreNodeStore store, uint32 target, uint8 level, const TreesShape *shape,
							 TreesBuildBuilder *result)
{
	trees_build_Builder_at_level_frame_0_t frame = {.store = store, .target = target, .level = level,
	.shape = shape, .result = result};

	std_landing(&frame, trees_build_Builder_at_level_body_0, trees_build_Builder_at_level_pad_0);
}

/* trees_build_Builder_add_frame_0_t is add's state across its pad: the entry it takes, while live. */
typedef struct trees_build_Builder_add_frame_0_t
{
	TreesBuildBuilder *self;
	HostIoIo   *io;
	TreesBuildEntry *entry;
	bool		entry_live;
	ChunksErrorError *error;
	bool		ok;
} trees_build_Builder_add_frame_0_t;

/* trees_build_Builder_add_body_0 is add's body: the entry's key checked against the last, then the entry pushed. */
static void
trees_build_Builder_add_body_0(void *frame_)
{
	trees_build_Builder_add_frame_0_t *frame = frame_;
	TreesBuildBuilder *self = frame->self;
	TreesBuildLevel *first;
	TreesBuildItem item;

	std_index_check(0, self->levels.len, RUST_FILE, 237, 40);
	first = &STD_VEC_AT(&self->levels, TreesBuildLevel, 0);
	if (first->items.len != 0)
	{
		const TreesBuildItem *last = &STD_VEC_AT(&first->items, TreesBuildItem,
												 std_wrapping_sub_usize(first->items.len, 1));
		StdBytes	last_key = trees_build_Item_key(last);
		StdOrdering ordering;

		if (!trees_KeyOrder_compare(&self->shape.order, frame->io, last_key.ptr, last_key.len,
									frame->entry->key.ptr, frame->entry->key.len, &ordering, frame->error))
		{
			frame->entry_live = false;
			trees_build_Entry_drop(frame->entry);
			frame->ok = false;
			return;
		}
		if (ordering != STD_ORDERING_LESS)
		{
			static const StdFmtPiece pieces[] = {STD_FMT_LIT("a tree build is fed a key that sorts before the last")};
			StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
			frame->error->u.invariant = message;
			frame->entry_live = false;
			trees_build_Entry_drop(frame->entry);
			frame->ok = false;
			return;
		}
	}
	item.tag = TREES_BUILD_ITEM_LEAF;
	item.u.leaf = *frame->entry;
	frame->entry_live = false;
	frame->ok = trees_build_Builder_push(self, frame->io, 0, &item, frame->error);
}

/* trees_build_Builder_add_pad_0 is add's pad: the entry, while live. */
static void
trees_build_Builder_add_pad_0(void *frame_)
{
	trees_build_Builder_add_frame_0_t *frame = frame_;

	if (frame->entry_live)
		trees_build_Entry_drop(frame->entry);
}

/* trees_build_Builder_add is Builder::add: a leaf entry added, which must sort after every entry added before it; it takes the entry. */
bool
trees_build_Builder_add(TreesBuildBuilder *self, HostIoIo *io, TreesBuildEntry *entry, ChunksErrorError *error)
{
	trees_build_Builder_add_frame_0_t frame = {.self = self, .io = io, .entry = entry, .entry_live = true,
	.error = error};

	std_landing(&frame, trees_build_Builder_add_body_0, trees_build_Builder_add_pad_0);
	return frame.ok;
}

static bool trees_build_Builder_finish_node(TreesBuildBuilder *self, HostIoIo *io, size_t level,
											ChunksErrorError *error);

/* trees_build_Builder_push_frame_0_t is push's state across its pad: the item it takes, while live. */
typedef struct trees_build_Builder_push_frame_0_t
{
	TreesBuildBuilder *self;
	HostIoIo   *io;
	size_t		level;
	TreesBuildItem *item;
	bool		item_live;
	ChunksErrorError *error;
	bool		ok;
} trees_build_Builder_push_frame_0_t;

/* trees_build_Builder_push_body_0 is push's body: the item checked and added to the level, the level's node finished where the splitter says. */
static void
trees_build_Builder_push_body_0(void *frame_)
{
	trees_build_Builder_push_frame_0_t *frame = frame_;
	TreesBuildBuilder *self = frame->self;
	TreesBuildLevel *l;
	StdBytes	key;
	uint64		size;
	bool		ends;

	if (!trees_build_Item_check(frame->item, frame->error))
	{
		frame->item_live = false;
		trees_build_Item_drop(frame->item);
		frame->ok = false;
		return;
	}
	std_index_check(frame->level, self->levels.len, RUST_FILE, 247, 33);
	l = &STD_VEC_AT(&self->levels, TreesBuildLevel, frame->level);
	key = trees_build_Item_key(frame->item);
	size = trees_build_Item_size(frame->item);
	ends = trees_chunker_Splitter_add(&l->splitter, key.ptr, key.len, size);
	std_vec_push_Checked(&l->items, frame->item, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
	frame->item_live = false;
	frame->ok = !ends || trees_build_Builder_finish_node(self, frame->io, frame->level, frame->error);
}

/* trees_build_Builder_push_pad_0 is push's pad: the item, while live. */
static void
trees_build_Builder_push_pad_0(void *frame_)
{
	trees_build_Builder_push_frame_0_t *frame = frame_;

	if (frame->item_live)
		trees_build_Item_drop(frame->item);
}

/* trees_build_Builder_push is Builder::push: an item added to the level, the level's node finished where the splitter says; it takes the item. */
bool
trees_build_Builder_push(TreesBuildBuilder *self, HostIoIo *io, size_t level, TreesBuildItem *item,
						 ChunksErrorError *error)
{
	trees_build_Builder_push_frame_0_t frame = {.self = self, .io = io, .level = level, .item = item,
	.item_live = true, .error = error};

	std_landing(&frame, trees_build_Builder_push_body_0, trees_build_Builder_push_pad_0);
	return frame.ok;
}

/* trees_build_Builder_finish_node_frame_0_t is finish_node's state across its pad: the items it took and the child it wrote, while live. */
typedef struct trees_build_Builder_finish_node_frame_0_t
{
	TreesBuildBuilder *self;
	HostIoIo   *io;
	size_t		level;
	ChunksErrorError *error;
	bool		ok;
	StdVec		items;
	bool		items_live;
	TreesBuildItem child;
	bool		child_live;
} trees_build_Builder_finish_node_frame_0_t;

/* trees_build_Builder_finish_node_body_0 is finish_node's body: the level's node written and added to the level above. */
static void
trees_build_Builder_finish_node_body_0(void *frame_)
{
	trees_build_Builder_finish_node_frame_0_t *frame = frame_;
	TreesBuildBuilder *self = frame->self;
	size_t		level = frame->level;
	TreesBuildLevel *l;
	size_t		above;

	std_index_check(level, self->levels.len, RUST_FILE, 258, 52);
	l = &STD_VEC_AT(&self->levels, TreesBuildLevel, level);
	frame->items = l->items;
	frame->items_live = true;
	std_vec_new(&l->items, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
	if (!trees_build_write_node(frame->io, self->store, (uint8) level, frame->items.ptr, frame->items.len,
								&self->shape, &frame->child, frame->error))
	{
		frame->items_live = false;
		trees_build_VecItem_drop(&frame->items);
		frame->ok = false;
		return;
	}
	frame->child_live = true;
	std_index_check(level, self->levels.len, RUST_FILE, 260, 20);
	l = &STD_VEC_AT(&self->levels, TreesBuildLevel, level);
	l->finished = STD_ADD(u64, l->finished, 1, 260, 9);
	above = STD_ADD(usize, level, 1, 261, 33);
	if (self->levels.len == above)
	{
		uint8		next = STD_ADD(u8, (uint8) level, 1, 262, 24);
		TreesBuildLevel added;

		if (next > TREES_CHUNKER_MAX_LEVEL)
		{
			static const char text[] = "tree is too deep";

			*frame->error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
			frame->child_live = false;
			trees_build_Item_drop(&frame->child);
			frame->items_live = false;
			trees_build_VecItem_drop(&frame->items);
			frame->ok = false;
			return;
		}
		added.splitter = trees_chunker_Splitter_new(next, self->target);
		std_vec_new(&added.items, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
		added.finished = 0;
		std_vec_push_Checked(&self->levels, &added, sizeof(TreesBuildLevel), _Alignof(TreesBuildLevel));
	}
	above = STD_ADD(usize, level, 1, 268, 23);
	frame->child_live = false;
	frame->ok = trees_build_Builder_push(self, frame->io, above, &frame->child, frame->error);
	frame->items_live = false;
	trees_build_VecItem_drop(&frame->items);
}

/* trees_build_Builder_finish_node_pad_0 is finish_node's pad: the child and the items, while live. */
static void
trees_build_Builder_finish_node_pad_0(void *frame_)
{
	trees_build_Builder_finish_node_frame_0_t *frame = frame_;

	if (frame->child_live)
		trees_build_Item_drop(&frame->child);
	if (frame->items_live)
		trees_build_VecItem_drop(&frame->items);
}

/* trees_build_Builder_finish_node is Builder::finish_node: the level's node written and added to the level above. */
static bool
trees_build_Builder_finish_node(TreesBuildBuilder *self, HostIoIo *io, size_t level, ChunksErrorError *error)
{
	trees_build_Builder_finish_node_frame_0_t frame = {.self = self, .io = io, .level = level, .error = error};

	std_landing(&frame, trees_build_Builder_finish_node_body_0, trees_build_Builder_finish_node_pad_0);
	return frame.ok;
}

/* trees_build_Builder_drop is Builder's drop glue: its shape's, then its levels'. */
void
trees_build_Builder_drop(TreesBuildBuilder *self)
{
	trees_Shape_drop(&self->shape);
	for (size_t k = 0, k_len = self->levels.len; k < k_len; k++)
		trees_build_Level_drop(&STD_VEC_AT(&self->levels, TreesBuildLevel, k));
	std_vec_free_Checked(&self->levels, sizeof(TreesBuildLevel), _Alignof(TreesBuildLevel));
}

/* trees_build_Builder_finish_into_frame_0_t is finish_into's state across its pad: the builder it took, the items it took, and the item it wrote, while live. */
typedef struct trees_build_Builder_finish_into_frame_0_t
{
	TreesBuildBuilder *self;
	HostIoIo   *io;
	TreesStoreTree *result_0;
	TreesStoreNodeStore *result_1;
	ChunksErrorError *error;
	bool		ok;
	bool		self_live;
	StdVec		items;
	bool		items_live;
	TreesBuildItem written;
	bool		written_live;
} trees_build_Builder_finish_into_frame_0_t;

/* trees_build_Builder_finish_into_body_0 is finish_into's body: each level from the base finished until one node is the root. */
static void
trees_build_Builder_finish_into_body_0(void *frame_)
{
	trees_build_Builder_finish_into_frame_0_t *frame = frame_;
	TreesBuildBuilder *self = frame->self;
	size_t		level = self->base;

	for (;;)
	{
		TreesBuildLevel *l;
		uint64		nodes;

		std_index_check(level, self->levels.len, RUST_FILE, 289, 33);
		l = &STD_VEC_AT(&self->levels, TreesBuildLevel, level);
		nodes = STD_ADD(u64, l->finished, (uint64) (l->items.len != 0), 290, 25);
		if (nodes <= 1 && (l->finished == 0 || STD_ADD(usize, level, 1, 291, 50) == self->levels.len))
		{
			std_index_check(level, self->levels.len, RUST_FILE, 292, 60);
			l = &STD_VEC_AT(&self->levels, TreesBuildLevel, level);
			frame->items = l->items;
			frame->items_live = true;
			std_vec_new(&l->items, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
			if (!trees_build_write_node(frame->io, self->store, (uint8) level, frame->items.ptr, frame->items.len,
										&self->shape, &frame->written, frame->error))
			{
				frame->items_live = false;
				trees_build_VecItem_drop(&frame->items);
				frame->self_live = false;
				trees_build_Builder_drop(self);
				frame->ok = false;
				return;
			}
			frame->written_live = true;
			if (frame->written.tag != TREES_BUILD_ITEM_CHILD)
			{
				static const char text[] = "internal error: entered unreachable code: write_node returns an internal item";

				std_panic(RUST_FILE, 295, 57, text, sizeof(text) - 1);
			}
			*frame->result_0 = frame->written.u.child.tree;
			*frame->result_1 = self->store;
			frame->written_live = false;
			trees_build_Item_drop(&frame->written);
			frame->items_live = false;
			trees_build_VecItem_drop(&frame->items);
			frame->self_live = false;
			trees_build_Builder_drop(self);
			frame->ok = true;
			return;
		}
		if (nodes == 1)
		{
			size_t		above = STD_ADD(usize, level, 1, 299, 43);
			const TreesBuildLevel *up;

			std_index_check(above, self->levels.len, RUST_FILE, 299, 42);
			up = &STD_VEC_AT(&self->levels, TreesBuildLevel, above);
			if (up->items.len != 1 || STD_VEC_AT(&up->items, TreesBuildItem, 0).tag != TREES_BUILD_ITEM_CHILD)
			{
				static const char text[] = "internal error: entered unreachable code: a level with one finished node has one item above it";

				std_panic(RUST_FILE, 301, 26, text, sizeof(text) - 1);
			}
			*frame->result_0 = STD_VEC_AT(&up->items, TreesBuildItem, 0).u.child.tree;
			*frame->result_1 = self->store;
			frame->self_live = false;
			trees_build_Builder_drop(self);
			frame->ok = true;
			return;
		}
		std_index_check(level, self->levels.len, RUST_FILE, 304, 28);
		l = &STD_VEC_AT(&self->levels, TreesBuildLevel, level);
		if (l->items.len != 0 && !trees_build_Builder_finish_node(self, frame->io, level, frame->error))
		{
			frame->self_live = false;
			trees_build_Builder_drop(self);
			frame->ok = false;
			return;
		}
		level = STD_ADD(usize, level, 1, 307, 13);
	}
}

/* trees_build_Builder_finish_into_pad_0 is finish_into's pad: the item it wrote, the items, and the builder, while live. */
static void
trees_build_Builder_finish_into_pad_0(void *frame_)
{
	trees_build_Builder_finish_into_frame_0_t *frame = frame_;

	if (frame->written_live)
		trees_build_Item_drop(&frame->written);
	if (frame->items_live)
		trees_build_VecItem_drop(&frame->items);
	if (frame->self_live)
		trees_build_Builder_drop(frame->self);
}

/* trees_build_Builder_finish_into is Builder::finish_into: finish_tree that gives the store back; it takes the builder. */
static bool
trees_build_Builder_finish_into(TreesBuildBuilder *self, HostIoIo *io, TreesStoreTree *result_0,
								TreesStoreNodeStore *result_1, ChunksErrorError *error)
{
	trees_build_Builder_finish_into_frame_0_t frame = {.self = self, .io = io, .result_0 = result_0,
	.result_1 = result_1, .error = error, .self_live = true};

	std_landing(&frame, trees_build_Builder_finish_into_body_0, trees_build_Builder_finish_into_pad_0);
	return frame.ok;
}

/*
 * trees_build_Builder_finish is Builder::finish: the nodes still being built
 * written, returning the tree, waiting to be published; it takes the
 * builder. branded is the Branded table of the builder's store, the bound
 * finish adds to S.
 */
bool
trees_build_Builder_finish(TreesBuildBuilder *self, HostIoIo *io, const TreesStoreBrandedOps *branded,
						   TreesStoreFresh *result, ChunksErrorError *error)
{
	TreesStoreTree tree;
	TreesStoreNodeStore store;
	TreesStoreBranded as_branded;

	if (!trees_build_Builder_finish_into(self, io, &tree, &store, error))
		return false;
	as_branded.ops = branded;
	as_branded.self = store.self;
	return trees_store_rooted(io, as_branded, tree, result, error);
}

/* trees_build_Builder_finish_tree is Builder::finish_tree: finish for this crate's own trees; it takes the builder. */
bool
trees_build_Builder_finish_tree(TreesBuildBuilder *self, HostIoIo *io, TreesStoreTree *result,
								ChunksErrorError *error)
{
	TreesStoreNodeStore store;

	return trees_build_Builder_finish_into(self, io, result, &store, error);
}
