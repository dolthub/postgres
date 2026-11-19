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
 * The C port of trees' mutate.rs: changing a tree by a batch of edits,
 * which gives the same tree as building the changed entries from scratch.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/address.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/node.h"
#ifdef DOLTGRES_TEST_TREES
#include "doltgres/host/local.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "mutate.rs"

static void trees_mutate_VecItem_IntoIter_drop(StdVec *self, size_t at);
static bool trees_mutate_Merger_apply(TreesMutateMerger *self, HostIoIo *io, TreesStoreNodeStore store, bool last,
									  ChunksErrorError *error);
static bool trees_mutate_Level_apply(TreesMutateLevel *self, HostIoIo *io, TreesStoreNodeStore store,
									 TreesStoreTree tree, uint32 target, const TreesShape *shape, bool last,
									 StdVec *result, ChunksErrorError *error);
static bool trees_mutate_skip_old(void);
static uint64 trees_mutate_old_weight(const TreesCursorCursor *cursor, uint8 level);
static bool trees_mutate_old_item(TreesCursorCursor *cursor, uint8 level, StdArcInner *const *stamps,
								  TreesBuildItem *result, ChunksErrorError *error);
static bool trees_mutate_node_stamps(HostIoIo *io, TreesStoreNodeStore store, const TreesCursorCursor *cursor,
									 StdArcInner **result, ChunksErrorError *error);
static bool trees_mutate_new_root(HostIoIo *io, TreesStoreNodeStore store, uint32 target, uint8 level, StdVec *nodes,
								  const TreesShape *shape, TreesStoreTree *result, ChunksErrorError *error);

/* trees_mutate_Edit_Clone_clone is Edit's derived Clone: the variant's field cloned. */
void
trees_mutate_Edit_Clone_clone(const void *self_, void *result_)
{
	const TreesMutateEdit *self = self_;
	TreesMutateEdit *result = result_;

	result->tag = self->tag;
	if (self->tag == TREES_MUTATE_EDIT_PUT)
		trees_build_Entry_Clone_clone(&self->u.put, &result->u.put);
	else
		std_vec_clone(&result->u.delete, &self->u.delete, 1, 1);
}

/* trees_mutate_Edit_Debug_fmt is Edit's derived Debug. */
bool
trees_mutate_Edit_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesMutateEdit *self = value;
	StdDebugTuple t;

	if (self->tag == TREES_MUTATE_EDIT_PUT)
	{
		t = std_fmt_debug_tuple(f, "Put", 3);
		std_DebugTuple_field(&t, &self->u.put, trees_build_Entry_Debug_fmt);
	}
	else
	{
		t = std_fmt_debug_tuple(f, "Delete", 6);
		std_DebugTuple_field(&t, &self->u.delete, chunks_chunk_VecU8_Debug_fmt);
	}
	return std_DebugTuple_finish(&t);
}

/* trees_mutate_Edit_PartialEq_eq is Edit's derived PartialEq: the same variant with equal fields. */
bool
trees_mutate_Edit_PartialEq_eq(const TreesMutateEdit *self, const TreesMutateEdit *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag == TREES_MUTATE_EDIT_PUT)
		return trees_build_Entry_PartialEq_eq(&self->u.put, &other->u.put);
	return trees_store_VecU8_PartialEq_eq(&self->u.delete, &other->u.delete);
}

/* trees_mutate_Edit_drop is Edit's drop glue: the variant's field's. */
void
trees_mutate_Edit_drop(void *self_)
{
	TreesMutateEdit *self = self_;

	if (self->tag == TREES_MUTATE_EDIT_PUT)
		trees_build_Entry_drop(&self->u.put);
	else
		std_vec_free_Checked(&self->u.delete, 1, 1);
}

/* trees_mutate_Edit_key is Edit::key: the key the edit changes. */
StdBytes
trees_mutate_Edit_key(const TreesMutateEdit *self)
{
	StdBytes	key;

	if (self->tag == TREES_MUTATE_EDIT_PUT)
	{
		key.ptr = self->u.put.key.ptr;
		key.len = self->u.put.key.len;
	}
	else
	{
		key.ptr = self->u.delete.ptr;
		key.len = self->u.delete.len;
	}
	return key;
}

/* trees_mutate_VecEdit_drop is Vec<Edit>'s drop glue: each edit's, then the buffer. */
static void
trees_mutate_VecEdit_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		trees_mutate_Edit_drop(&STD_VEC_AT(self, TreesMutateEdit, k));
	std_vec_free_Checked(self, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
}

/*
 * trees_mutate_mutate is mutate: the edits, sorted in the tree's key order
 * with each key once, applied to a tree of the shape, writing new nodes with
 * the target size, the root returned waiting to be published; it takes the
 * edits.
 */
bool
trees_mutate_mutate(HostIoIo *io, TreesStoreBranded store, TreesStoreNamed tree, uint32 target, StdVec *edits,
					const TreesShape *shape, TreesStoreFresh *result, ChunksErrorError *error)
{
	TreesStoreNodeStore node_store = {&store.ops->node_store, store.self};
	TreesStoreTree changed;

	if (!trees_mutate_mutate_tree(io, node_store, trees_store_name(tree), target, edits, shape, &changed, error))
		return false;
	return trees_store_rooted(io, store, changed, result, error);
}

/* trees_mutate_mutate_tree_frame_0_t is mutate_tree's state across its pad: the edits it takes and the merger, while live. */
typedef struct trees_mutate_mutate_tree_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreTree tree;
	uint32		target;
	StdVec	   *edits;
	bool		edits_live;
	const TreesShape *shape;
	TreesStoreTree *result;
	ChunksErrorError *error;
	bool		ok;
	TreesMutateMerger merger;
	bool		merger_live;
} trees_mutate_mutate_tree_frame_0_t;

/* trees_mutate_mutate_tree_body_0 is mutate_tree's body: a merger of the tree fed the edits and finished. */
static void
trees_mutate_mutate_tree_body_0(void *frame_)
{
	trees_mutate_mutate_tree_frame_0_t *frame = frame_;

	if (!trees_mutate_Merger_new(frame->io, frame->store, TREES_STORE_TREE_NAMED(&frame->tree), frame->target,
								 frame->shape, &frame->merger, frame->error))
	{
		frame->edits_live = false;
		trees_mutate_VecEdit_drop(frame->edits);
		frame->ok = false;
		return;
	}
	frame->merger_live = true;
	frame->edits_live = false;
	if (!trees_mutate_Merger_feed(&frame->merger, frame->io, frame->store, frame->edits, frame->error))
	{
		frame->merger_live = false;
		trees_mutate_Merger_drop(&frame->merger);
		frame->ok = false;
		return;
	}
	frame->merger_live = false;
	frame->ok = trees_mutate_Merger_finish_tree(&frame->merger, frame->io, frame->store, frame->result,
												frame->error);
}

/* trees_mutate_mutate_tree_pad_0 is mutate_tree's pad: the merger and the edits, while live. */
static void
trees_mutate_mutate_tree_pad_0(void *frame_)
{
	trees_mutate_mutate_tree_frame_0_t *frame = frame_;

	if (frame->merger_live)
		trees_mutate_Merger_drop(&frame->merger);
	if (frame->edits_live)
		trees_mutate_VecEdit_drop(frame->edits);
}

/* trees_mutate_mutate_tree is mutate_tree, mutate for this crate's own trees; it takes the edits. */
bool
trees_mutate_mutate_tree(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree, uint32 target, StdVec *edits,
						 const TreesShape *shape, TreesStoreTree *result, ChunksErrorError *error)
{
	trees_mutate_mutate_tree_frame_0_t frame = {.io = io, .store = store, .tree = tree, .target = target,
	.edits = edits, .edits_live = true, .shape = shape, .result = result, .error = error};

	std_landing(&frame, trees_mutate_mutate_tree_body_0, trees_mutate_mutate_tree_pad_0);
	return frame.ok;
}


/* trees_mutate_TupleVecU8OptionItem_drop is (Vec<u8>, Option<Item>)'s drop glue: its key's, then its item's, if any. */
static void
trees_mutate_TupleVecU8OptionItem_drop(void *self_)
{
	trees_TupleVecU8OptionItem_t *self = self_;

	std_vec_free_Checked(&self->f0, 1, 1);
	if (self->has_f1)
		trees_build_Item_drop(&self->f1);
}

/* trees_mutate_VecTupleVecU8OptionItem_drop is LevelEdits' drop glue, Vec<(Vec<u8>, Option<Item>)>'s: each edit's, then the buffer. */
static void
trees_mutate_VecTupleVecU8OptionItem_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		trees_mutate_TupleVecU8OptionItem_drop(&STD_VEC_AT(self, trees_TupleVecU8OptionItem_t, k));
	std_vec_free_Checked(self, sizeof(trees_TupleVecU8OptionItem_t), _Alignof(trees_TupleVecU8OptionItem_t));
}

/* trees_mutate_VecTupleVecU8Tree_drop is Vec<(Vec<u8>, Tree)>'s drop glue: each key's, then the buffer. */
static void
trees_mutate_VecTupleVecU8Tree_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(self, trees_TupleVecU8Tree_t, k).f0, 1, 1);
	std_vec_free_Checked(self, sizeof(trees_TupleVecU8Tree_t), _Alignof(trees_TupleVecU8Tree_t));
}

/* trees_mutate_Run_drop is Run's drop glue: its stamps', splitter's, pending items', consumed nodes', and produced items'. */
static void
trees_mutate_Run_drop(TreesMutateRun *self)
{
	trees_cursor_ArcStamps_drop(self->stamps);
	trees_chunker_Splitter_drop(&self->splitter);
	trees_build_VecItem_drop(&self->pending);
	trees_mutate_VecTupleVecU8Tree_drop(&self->consumed);
	trees_build_VecItem_drop(&self->produced);
}

/* trees_mutate_Level_drop is Level's drop glue: its cursor's, edits', run's, and made nodes', each if any. */
static void
trees_mutate_Level_drop(TreesMutateLevel *self)
{
	if (self->has_cursor)
		trees_cursor_Cursor_drop(&self->cursor);
	std_vec_deque_drop(&self->edits, sizeof(trees_TupleVecU8OptionItem_t), _Alignof(trees_TupleVecU8OptionItem_t),
					   trees_mutate_TupleVecU8OptionItem_drop);
	if (self->has_run)
		trees_mutate_Run_drop(&self->run);
	if (self->has_made)
		trees_build_VecItem_drop(&self->made);
}

/* trees_mutate_Merger_drop is Merger's drop glue: its shape's, then its levels'. */
void
trees_mutate_Merger_drop(TreesMutateMerger *self)
{
	trees_Shape_drop(&self->shape);
	for (size_t k = 0, k_len = self->levels.len; k < k_len; k++)
		trees_mutate_Level_drop(&STD_VEC_AT(&self->levels, TreesMutateLevel, k));
	std_vec_free_Checked(&self->levels, sizeof(TreesMutateLevel), _Alignof(TreesMutateLevel));
}

/*
 * trees_mutate_Merger_new is Merger::new: a merger of edits into a tree of
 * the shape, writing new nodes with the target size, a level for each of
 * the tree's from the leaves to its root's, a Map<RangeInclusive<u8>>'s
 * collect.
 */
bool
trees_mutate_Merger_new(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint32 target,
						const TreesShape *shape, TreesMutateMerger *result, ChunksErrorError *error)
{
	TreesStoreTree named = trees_store_name(tree);
	TreesNodeFlat read;
	TreesNodeNode node;
	uint8		top;
	size_t		steps;
	StdSizeHint hint;

	if (!store.ops->read(store.self, io, &named.address, named.locator, &read, error))
		return false;
	node = trees_node_Flat_node(&read);
	top = trees_node_Node_level(&node);
	trees_node_Flat_drop(&read);
	result->tree = named;
	result->target = target;
	trees_Shape_Clone_clone(shape, &result->shape);
	steps = std_wrapping_add_usize((size_t) top, 1);
	hint.lower = steps;
	hint.has_upper = true;
	hint.upper = steps;
	std_vec_with_capacity_trusted(&result->levels, hint, sizeof(TreesMutateLevel), _Alignof(TreesMutateLevel));
	std_vec_extend_trusted_reserve(&result->levels, hint, sizeof(TreesMutateLevel), _Alignof(TreesMutateLevel));
	{
		uint8		l = 0;
		bool		l_done = false;

		for (; !l_done; l_done = (l == top), l = l_done ? l : std_forward_unchecked_u8_Checked(l, 1))
		{
			TreesMutateLevel *made = &STD_VEC_AT(&result->levels, TreesMutateLevel, result->levels.len);

			made->level = l;
			made->has_cursor = false;
			std_vec_deque_new(&made->edits, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
			made->has_run = false;
			made->has_made = l == top;
			if (made->has_made)
				std_vec_new(&made->made, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
			result->levels.len = std_wrapping_add_usize(result->levels.len, 1);
		}
	}
	result->reached = false;
	return true;
}


/* trees_mutate_Merger_feed_frame_0_t is feed's state across its pad: the edits it takes as their IntoIter, and the edit being mapped, while live. */
typedef struct trees_mutate_Merger_feed_frame_0_t
{
	TreesMutateMerger *self;
	HostIoIo   *io;
	TreesStoreNodeStore store;
	StdVec	   *edits;
	bool		edits_live;
	size_t		edits_at;
	TreesMutateEdit edit;
	bool		edit_live;
	ChunksErrorError *error;
	bool		ok;
} trees_mutate_Merger_feed_frame_0_t;

/* trees_mutate_Merger_feed_body_0 is feed's body: the edits checked for order, moved to the leaves' level's edits, and applied as far as they decide the tree. */
static void
trees_mutate_Merger_feed_body_0(void *frame_)
{
	trees_mutate_Merger_feed_frame_0_t *frame = frame_;
	TreesMutateMerger *self = frame->self;
	StdWindows	windows = std_slice_windows(frame->edits->ptr, frame->edits->len, 2, RUST_FILE, 140, 27);
	const void *window_;
	size_t		window_len;
	StdVecDeque *edits;

	while (std_slice_Windows_next(&windows, sizeof(TreesMutateEdit), &window_, &window_len))
	{
		const TreesMutateEdit *pair = window_;
		StdBytes	a;
		StdBytes	b;
		StdOrdering ordering;

		std_index_check(0, window_len, RUST_FILE, 141, 56);
		a = trees_mutate_Edit_key(&pair[0]);
		std_index_check(1, window_len, RUST_FILE, 141, 71);
		b = trees_mutate_Edit_key(&pair[1]);
		if (!trees_KeyOrder_compare(&self->shape.order, frame->io, a.ptr, a.len, b.ptr, b.len, &ordering,
									frame->error))
		{
			frame->edits_live = false;
			trees_mutate_VecEdit_drop(frame->edits);
			frame->ok = false;
			return;
		}
		if (ordering != STD_ORDERING_LESS)
		{
			static const StdFmtPiece pieces[] = {STD_FMT_LIT("edits must be sorted with each key once")};
			StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
			frame->error->u.invariant = message;
			frame->edits_live = false;
			trees_mutate_VecEdit_drop(frame->edits);
			frame->ok = false;
			return;
		}
	}
	std_index_check(0, self->levels.len, RUST_FILE, 144, 20);
	edits = &STD_VEC_AT(&self->levels, TreesMutateLevel, 0).edits;
	std_vec_deque_reserve_Checked(edits, frame->edits->len, sizeof(trees_TupleVecU8OptionItem_t),
								  _Alignof(trees_TupleVecU8OptionItem_t));
	for (frame->edits_at = 0; frame->edits_at < frame->edits->len;)
	{
		trees_TupleVecU8OptionItem_t mapped;

		frame->edit = STD_VEC_AT(frame->edits, TreesMutateEdit, frame->edits_at);
		frame->edits_at = std_wrapping_add_usize(frame->edits_at, 1);
		frame->edit_live = true;
		if (frame->edit.tag == TREES_MUTATE_EDIT_PUT)
		{
			std_vec_clone(&mapped.f0, &frame->edit.u.put.key, 1, 1);
			mapped.has_f1 = true;
			mapped.f1.tag = TREES_BUILD_ITEM_LEAF;
			mapped.f1.u.leaf = frame->edit.u.put;
		}
		else
		{
			mapped.f0 = frame->edit.u.delete;
			mapped.has_f1 = false;
		}
		frame->edit_live = false;
		memcpy(std_vec_deque_back_slot(edits, sizeof(mapped)), &mapped, sizeof(mapped));
		edits->len = std_wrapping_add_usize(edits->len, 1);
	}
	frame->edits_live = false;
	std_vec_free_Checked(frame->edits, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	frame->ok = trees_mutate_Merger_apply(self, frame->io, frame->store, false, frame->error);
}

/* trees_mutate_Merger_feed_pad_0 is feed's pad: the edit being mapped, then the edits its IntoIter has left, while live. */
static void
trees_mutate_Merger_feed_pad_0(void *frame_)
{
	trees_mutate_Merger_feed_frame_0_t *frame = frame_;

	if (frame->edit_live)
		trees_mutate_Edit_drop(&frame->edit);
	if (frame->edits_live)
	{
		for (size_t k = 0, k_len = std_wrapping_sub_usize(frame->edits->len, frame->edits_at); k < k_len; k++)
			trees_mutate_Edit_drop(&STD_VEC_AT(frame->edits, TreesMutateEdit, std_wrapping_add_usize(frame->edits_at, k)));
		std_vec_free_Checked(frame->edits, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	}
}

/*
 * trees_mutate_Merger_feed is Merger::feed: edits sorted in the tree's key
 * order with each key once, all after the keys fed before, applied as far
 * as the edits fed so far decide the tree; it takes the edits.
 */
bool
trees_mutate_Merger_feed(TreesMutateMerger *self, HostIoIo *io, TreesStoreNodeStore store, StdVec *edits,
						 ChunksErrorError *error)
{
	trees_mutate_Merger_feed_frame_0_t frame = {.self = self, .io = io, .store = store, .edits = edits,
	.edits_live = true, .error = error};

	std_landing(&frame, trees_mutate_Merger_feed_body_0, trees_mutate_Merger_feed_pad_0);
	return frame.ok;
}

/*
 * trees_mutate_Merger_finish is Merger::finish: what is left of the edits
 * applied, the changed tree returned waiting to be published; it takes the
 * merger.
 */
bool
trees_mutate_Merger_finish(TreesMutateMerger *self, HostIoIo *io, TreesStoreBranded store, TreesStoreFresh *result,
						   ChunksErrorError *error)
{
	TreesStoreNodeStore node_store = {&store.ops->node_store, store.self};
	TreesStoreTree tree;

	if (!trees_mutate_Merger_finish_tree(self, io, node_store, &tree, error))
		return false;
	return trees_store_rooted(io, store, tree, result, error);
}

/* trees_mutate_Merger_finish_tree_frame_0_t is finish_tree's state across its pad: the merger it takes, the level it pops, and the nodes it keeps of it, while live. */
typedef struct trees_mutate_Merger_finish_tree_frame_0_t
{
	TreesMutateMerger *self;
	bool		self_live;
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreTree *result;
	ChunksErrorError *error;
	bool		ok;
	TreesMutateLevel popped;
	bool		popped_live;
	StdVec		made;
	bool		made_live;
} trees_mutate_Merger_finish_tree_frame_0_t;

/* trees_mutate_Merger_finish_tree_body_0 is finish_tree's body: the rest applied, then the root's level's nodes made the new root, if an edit reached it. */
static void
trees_mutate_Merger_finish_tree_body_0(void *frame_)
{
	trees_mutate_Merger_finish_tree_frame_0_t *frame = frame_;
	TreesMutateMerger *self = frame->self;
	uint8		level;

	if (!trees_mutate_Merger_apply(self, frame->io, frame->store, true, frame->error))
	{
		frame->self_live = false;
		trees_mutate_Merger_drop(self);
		frame->ok = false;
		return;
	}
	if (self->levels.len != 0)
	{
		self->levels.len = std_wrapping_sub_usize(self->levels.len, 1);
		frame->popped = STD_VEC_AT(&self->levels, TreesMutateLevel, self->levels.len);
		frame->popped_live = true;
	}
	if (frame->popped_live && !self->reached)
	{
		frame->popped_live = false;
		trees_mutate_Level_drop(&frame->popped);
	}
	if (!frame->popped_live || !frame->popped.has_made)
	{
		if (frame->popped_live)
		{
			frame->popped_live = false;
			trees_mutate_Level_drop(&frame->popped);
		}
		*frame->result = self->tree;
		frame->self_live = false;
		trees_mutate_Merger_drop(self);
		frame->ok = true;
		return;
	}
	level = frame->popped.level;
	frame->made = frame->popped.made;
	frame->made_live = true;
	frame->popped.has_made = false;
	frame->popped_live = false;
	trees_mutate_Level_drop(&frame->popped);
	frame->made_live = false;
	frame->ok = trees_mutate_new_root(frame->io, frame->store, self->target, level, &frame->made, &self->shape,
									  frame->result, frame->error);
	frame->self_live = false;
	trees_mutate_Merger_drop(self);
}

/* trees_mutate_Merger_finish_tree_pad_0 is finish_tree's pad: the nodes kept, the popped level, and the merger, while live. */
static void
trees_mutate_Merger_finish_tree_pad_0(void *frame_)
{
	trees_mutate_Merger_finish_tree_frame_0_t *frame = frame_;

	if (frame->made_live)
		trees_build_VecItem_drop(&frame->made);
	if (frame->popped_live)
		trees_mutate_Level_drop(&frame->popped);
	if (frame->self_live)
		trees_mutate_Merger_drop(frame->self);
}

/* trees_mutate_Merger_finish_tree is Merger::finish_tree, finish for this crate's own trees; it takes the merger. */
bool
trees_mutate_Merger_finish_tree(TreesMutateMerger *self, HostIoIo *io, TreesStoreNodeStore store,
								TreesStoreTree *result, ChunksErrorError *error)
{
	trees_mutate_Merger_finish_tree_frame_0_t frame = {.self = self, .self_live = true, .io = io, .store = store,
	.result = result, .error = error};

	std_landing(&frame, trees_mutate_Merger_finish_tree_body_0, trees_mutate_Merger_finish_tree_pad_0);
	return frame.ok;
}

/*
 * trees_mutate_Merger_apply is Merger::apply: each level's edits applied,
 * the leaves' first, the edits each makes passed to the level above, as far
 * as the edits decide them, or all of them once the edits are all in.
 */
static bool
trees_mutate_Merger_apply(TreesMutateMerger *self, HostIoIo *io, TreesStoreNodeStore store, bool last,
						  ChunksErrorError *error)
{
	size_t		top = STD_SUB(usize, self->levels.len, 1, 169, 19);
	size_t		at = 0;
	bool		at_done = false;

	for (; !at_done; at_done = (at == top), at = at_done ? at : std_forward_unchecked_usize_Checked(at, 1))
	{
		StdVec		above;
		size_t		next;

		if (at == top)
		{
			std_index_check(at, self->levels.len, RUST_FILE, 171, 41);
			if (!std_vec_deque_is_empty(&STD_VEC_AT(&self->levels, TreesMutateLevel, at).edits))
				self->reached = true;
		}
		std_index_check(at, self->levels.len, RUST_FILE, 174, 36);
		if (!trees_mutate_Level_apply(&STD_VEC_AT(&self->levels, TreesMutateLevel, at), io, store, self->tree,
									  self->target, &self->shape, last, &above, error))
			return false;
		next = STD_ADD(usize, at, 1, 175, 53);
		if (next < self->levels.len)
			std_vec_deque_extend_vec_Checked(&STD_VEC_AT(&self->levels, TreesMutateLevel, next).edits, &above,
											 sizeof(trees_TupleVecU8OptionItem_t),
											 _Alignof(trees_TupleVecU8OptionItem_t),
											 trees_mutate_TupleVecU8OptionItem_drop);
		else
			trees_mutate_VecTupleVecU8OptionItem_drop(&above);
	}
	return true;
}


/* pin's instance at the merger's Cursor::advance_to_node and Cursor::next, R = Result<()>. */
#define TREES_PIN_SCOPE extern
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultUnit
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .error = error
#define TREES_PIN_RESULT_ARGS , frame->error
#include "doltgres/trees/pin_template.h"

/* pin's instance at the merger's Cursor::seek_node, R = Result<Cursor>. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultCursor
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , TreesCursorCursor *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS TreesCursorCursor *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* TreesMutateLevelApplyClosure0 is Level::apply's first pin scope's captures: the moved cursor, the key, and the order, by reference. */
typedef struct TreesMutateLevelApplyClosure0
{
	TreesCursorCursor *moved;
	StdBytes	key;
	const TreesKeyOrder *order;
} TreesMutateLevelApplyClosure0;

/* trees_mutate_Level_apply_closure_0 is the pin scope that moves the cursor on to the node of the next edit's key. */
static bool
trees_mutate_Level_apply_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	TreesMutateLevelApplyClosure0 *closure = captures;

	return trees_cursor_Cursor_advance_to_node(closure->moved, io, pin, closure->key.ptr, closure->key.len,
											   closure->order, error);
}

/* TreesMutateLevelApplyClosure1 is Level::apply's second pin scope's captures: the tree, the key, the level, and the order. */
typedef struct TreesMutateLevelApplyClosure1
{
	TreesStoreTree tree;
	StdBytes	key;
	uint8		level;
	const TreesKeyOrder *order;
} TreesMutateLevelApplyClosure1;

/* trees_mutate_Level_apply_closure_1 is the pin scope that seeks a cursor to the node at the level holding the next edit's key. */
static bool
trees_mutate_Level_apply_closure_1(void *captures, HostIoIo *io, TreesPinPin *pin, TreesCursorCursor *result,
								   ChunksErrorError *error)
{
	TreesMutateLevelApplyClosure1 *closure = captures;

	return trees_cursor_Cursor_seek_node(io, pin, TREES_STORE_TREE_NAMED(&closure->tree), closure->key.ptr,
										 closure->key.len, closure->level, closure->order, result, error);
}

/* trees_mutate_Level_apply_closure_2 is the pin scope that moves the cursor to its next item, the cursor its captures. */
static bool
trees_mutate_Level_apply_closure_2(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	return trees_cursor_Cursor_next(captures, io, pin, error);
}

/* trees_mutate_Level_apply_frame_0_t is Level::apply's state across its pad: what it holds outside the level, while live. */
typedef struct trees_mutate_Level_apply_frame_0_t
{
	TreesMutateLevel *self;
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreTree tree;
	uint32		target;
	const TreesShape *shape;
	bool		last;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		above;
	bool		above_live;
	TreesCursorCursor moved;
	bool		moved_live;
	StdArcInner *stamps;
	bool		stamps_live;
	TreesBuildItem item;
	bool		item_live;
	trees_TupleVecU8Tree_t passed;
	bool		passed_live;
	StdVec		taken;
	bool		taken_live;
	TreesBuildItem written;
	bool		written_live;
	TreesMutateRun run;
	bool		run_live;
	bool		run_rest_live;
	StdVec		consumed;
	bool		consumed_live;
	StdVec		produced;
	bool		produced_live;
} trees_mutate_Level_apply_frame_0_t;


/* trees_mutate_Level_apply_body_0 is Level::apply's body. */
static void
trees_mutate_Level_apply_body_0(void *frame_)
{
	trees_mutate_Level_apply_frame_0_t *frame = frame_;
	TreesMutateLevel *self = frame->self;
	const TreesKeyOrder *order = &frame->shape->order;
	uint8		level = self->level;

	std_vec_new(&frame->above, sizeof(trees_TupleVecU8OptionItem_t), _Alignof(trees_TupleVecU8OptionItem_t));
	frame->above_live = true;
	for (;;)
	{
		TreesCursorCursor *cursor;
		TreesMutateRun *run;

		if (!(self->has_cursor && self->has_run))
		{
			const trees_TupleVecU8OptionItem_t *front =
				std_vec_deque_front_Checked(&self->edits, sizeof(trees_TupleVecU8OptionItem_t));
			StdBytes	key;

			if (front == NULL)
			{
				frame->above_live = false;
				*frame->result = frame->above;
				frame->ok = true;
				return;
			}
			key.ptr = front->f0.ptr;
			key.len = front->f0.len;
			if (self->has_cursor)
			{
				TreesMutateLevelApplyClosure0 closure;

				frame->moved = self->cursor;
				self->has_cursor = false;
				frame->moved_live = true;
				closure.moved = &frame->moved;
				closure.key = key;
				closure.order = order;
				if (!trees_pin_pin_ResultUnit(frame->io, frame->store, trees_mutate_Level_apply_closure_0, &closure,
											  NULL, frame->error))
				{
					frame->moved_live = false;
					trees_cursor_Cursor_drop(&frame->moved);
					frame->above_live = false;
					trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
					frame->ok = false;
					return;
				}
			}
			else
			{
				TreesMutateLevelApplyClosure1 closure = {frame->tree, key, level, order};

				if (!trees_pin_pin_ResultCursor(frame->io, frame->store, trees_mutate_Level_apply_closure_1, &closure,
												NULL, &frame->moved, frame->error))
				{
					frame->above_live = false;
					trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
					frame->ok = false;
					return;
				}
				frame->moved_live = true;
			}
			if (!trees_mutate_node_stamps(frame->io, frame->store, &frame->moved, &frame->stamps, frame->error))
			{
				frame->moved_live = false;
				trees_cursor_Cursor_drop(&frame->moved);
				frame->above_live = false;
				trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
				frame->ok = false;
				return;
			}
			frame->stamps_live = true;
			self->run.stamps = frame->stamps;
			self->run.splitter = trees_chunker_Splitter_new(level, frame->target);
			std_vec_new(&self->run.pending, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
			std_vec_new(&self->run.consumed, sizeof(trees_TupleVecU8Tree_t), _Alignof(trees_TupleVecU8Tree_t));
			std_vec_new(&self->run.produced, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
			self->run.expected = 0;
			frame->stamps_live = false;
			self->has_run = true;
			self->cursor = frame->moved;
			frame->moved_live = false;
			self->has_cursor = true;
			continue;
		}
		cursor = &self->cursor;
		run = &self->run;
		for (;;)
		{
			bool		valid = trees_cursor_Cursor_is_valid(cursor);
			const trees_TupleVecU8OptionItem_t *front =
				std_vec_deque_front_Checked(&self->edits, sizeof(trees_TupleVecU8OptionItem_t));
			bool		has_order;
			StdOrdering edit_order = STD_ORDERING_EQUAL;
			bool		from_edit;
			bool		replaces_old;
			bool		old_end = false;

			if (front == NULL)
			{
				if (!frame->last)
				{
					frame->above_live = false;
					*frame->result = frame->above;
					frame->ok = true;
					return;
				}
				has_order = false;
			}
			else if (!valid)
			{
				has_order = true;
				edit_order = STD_ORDERING_LESS;
			}
			else
			{
				StdBytes	at = trees_cursor_Cursor_key(cursor);

				if (!trees_KeyOrder_compare(order, frame->io, front->f0.ptr, front->f0.len, at.ptr, at.len, &edit_order,
											frame->error))
				{
					frame->above_live = false;
					trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
					frame->ok = false;
					return;
				}
				has_order = true;
			}
			from_edit = has_order && edit_order != STD_ORDERING_GREATER;
			if (!from_edit && !valid)
				break;
			replaces_old = valid && (!from_edit || (has_order && edit_order == STD_ORDERING_EQUAL));
			if (from_edit)
			{
				trees_TupleVecU8OptionItem_t popped;

				if (std_vec_deque_pop_front_Checked(&self->edits, &popped, sizeof(popped)))
				{
					std_vec_free_Checked(&popped.f0, 1, 1);
					if (popped.has_f1)
					{
						frame->item = popped.f1;
						frame->item_live = true;
					}
				}
			}
			if (from_edit)
			{
				int64		weight = frame->item_live ? (int64) trees_build_Item_weight(&frame->item) : 0;

				run->expected = STD_ADD(i64, run->expected, weight, 237, 21);
			}
			if (replaces_old)
			{
				if (from_edit)
					run->expected = STD_SUB(i64, run->expected, (int64) trees_mutate_old_weight(cursor, level),
											242, 33);
				else if (!trees_mutate_skip_old())
				{
					if (!trees_mutate_old_item(cursor, level, &run->stamps, &frame->item, frame->error))
					{
						frame->above_live = false;
						trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
						frame->ok = false;
						return;
					}
					frame->item_live = true;
				}
				old_end = trees_cursor_Cursor_at_node_end(cursor);
				if (old_end)
				{
					TreesNodeNode node = trees_cursor_Cursor_node(cursor);
					StdBytes	key = trees_cursor_Cursor_key(cursor);

					run->expected = STD_ADD(i64, run->expected, (int64) trees_node_Node_total(&node), 248, 25);
					std_slice_to_vec_Checked(&frame->passed.f0, key.ptr, key.len, 1, 1);
					frame->passed_live = true;
					frame->passed.f1 = trees_cursor_Cursor_node_tree(cursor);
					std_vec_push_Checked(&run->consumed, &frame->passed, sizeof(trees_TupleVecU8Tree_t),
										 _Alignof(trees_TupleVecU8Tree_t));
					frame->passed_live = false;
				}
				if (!trees_pin_pin_ResultUnit(frame->io, frame->store, trees_mutate_Level_apply_closure_2, cursor, NULL,
											  frame->error))
				{
					if (frame->item_live)
					{
						frame->item_live = false;
						trees_build_Item_drop(&frame->item);
					}
					frame->above_live = false;
					trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
					frame->ok = false;
					return;
				}
				if (old_end && trees_cursor_Cursor_is_valid(cursor))
				{
					if (!trees_mutate_node_stamps(frame->io, frame->store, cursor, &frame->stamps, frame->error))
					{
						if (frame->item_live)
						{
							frame->item_live = false;
							trees_build_Item_drop(&frame->item);
						}
						frame->above_live = false;
						trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
						frame->ok = false;
						return;
					}
					frame->stamps_live = true;
					trees_cursor_ArcStamps_drop(run->stamps);
					run->stamps = frame->stamps;
					frame->stamps_live = false;
				}
			}
			if (frame->item_live)
			{
				StdBytes	key;
				uint64		size;
				bool		ends;

				if (!trees_build_Item_check(&frame->item, frame->error))
				{
					frame->item_live = false;
					trees_build_Item_drop(&frame->item);
					frame->above_live = false;
					trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
					frame->ok = false;
					return;
				}
				key = trees_build_Item_key(&frame->item);
				size = trees_build_Item_size(&frame->item);
				ends = trees_chunker_Splitter_add(&run->splitter, key.ptr, key.len, size);
				std_vec_push_Checked(&run->pending, &frame->item, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
				frame->item_live = false;
				if (ends)
				{
					frame->taken = run->pending;
					frame->taken_live = true;
					std_vec_new(&run->pending, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
					if (!trees_build_write_node(frame->io, frame->store, level, frame->taken.ptr, frame->taken.len,
												frame->shape, &frame->written, frame->error))
					{
						frame->taken_live = false;
						trees_build_VecItem_drop(&frame->taken);
						frame->above_live = false;
						trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
						frame->ok = false;
						return;
					}
					frame->written_live = true;
					std_vec_push_Checked(&run->produced, &frame->written, sizeof(TreesBuildItem),
										 _Alignof(TreesBuildItem));
					frame->written_live = false;
					frame->taken_live = false;
					trees_build_VecItem_drop(&frame->taken);
				}
			}
			if (old_end && run->pending.len == 0 && trees_chunker_Splitter_is_empty(&run->splitter) &&
				trees_cursor_Cursor_is_valid(cursor))
				break;
		}
		if (!self->has_run)
			continue;
		frame->run = self->run;
		self->has_run = false;
		frame->run_live = true;
		if (frame->run.pending.len != 0)
		{
			frame->taken = frame->run.pending;
			frame->taken_live = true;
			std_vec_new(&frame->run.pending, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
			if (!trees_build_write_node(frame->io, frame->store, level, frame->taken.ptr, frame->taken.len,
										frame->shape, &frame->written, frame->error))
			{
				frame->taken_live = false;
				trees_build_VecItem_drop(&frame->taken);
				frame->run_live = false;
				trees_mutate_Run_drop(&frame->run);
				frame->above_live = false;
				trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
				frame->ok = false;
				return;
			}
			frame->written_live = true;
			std_vec_push_Checked(&frame->run.produced, &frame->written, sizeof(TreesBuildItem),
								 _Alignof(TreesBuildItem));
			frame->written_live = false;
			frame->taken_live = false;
			trees_build_VecItem_drop(&frame->taken);
		}
		if (!trees_mutate_check_run(frame->io, frame->tree, level, frame->run.produced.ptr, frame->run.produced.len,
									frame->run.expected, order, frame->error))
		{
			frame->run_live = false;
			trees_mutate_Run_drop(&frame->run);
			frame->above_live = false;
			trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
			frame->ok = false;
			return;
		}
		frame->consumed = frame->run.consumed;
		frame->produced = frame->run.produced;
		frame->run_live = false;
		frame->run_rest_live = true;
		frame->consumed_live = true;
		frame->produced_live = true;
		if (!trees_mutate_run_edits(frame->io, frame->consumed.ptr, frame->consumed.len, frame->produced.ptr,
									frame->produced.len, order, &frame->above, frame->error))
		{
			frame->produced_live = false;
			trees_build_VecItem_drop(&frame->produced);
			frame->consumed_live = false;
			trees_mutate_VecTupleVecU8Tree_drop(&frame->consumed);
			frame->run_rest_live = false;
			trees_cursor_ArcStamps_drop(frame->run.stamps);
			trees_chunker_Splitter_drop(&frame->run.splitter);
			trees_build_VecItem_drop(&frame->run.pending);
			frame->above_live = false;
			trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
			frame->ok = false;
			return;
		}
		if (self->has_made)
		{
			std_vec_reserve_Checked(&self->made, frame->produced.len, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
			std_ptr_copy_nonoverlapping_Checked(frame->produced.ptr,
												STD_PTR_AT(self->made.ptr, self->made.len, sizeof(TreesBuildItem)),
												frame->produced.len, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
			self->made.len = std_wrapping_add_usize(self->made.len, frame->produced.len);
			frame->produced.len = 0;
			frame->produced_live = false;
			std_vec_free_Checked(&frame->produced, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
		}
		if (frame->produced_live)
		{
			frame->produced_live = false;
			trees_build_VecItem_drop(&frame->produced);
		}
		frame->consumed_live = false;
		trees_mutate_VecTupleVecU8Tree_drop(&frame->consumed);
		frame->run_rest_live = false;
		trees_cursor_ArcStamps_drop(frame->run.stamps);
		trees_chunker_Splitter_drop(&frame->run.splitter);
		trees_build_VecItem_drop(&frame->run.pending);
	}
}

/* trees_mutate_Level_apply_pad_0 is Level::apply's pad: what it holds, while live, in reverse order of declaration. */
static void
trees_mutate_Level_apply_pad_0(void *frame_)
{
	trees_mutate_Level_apply_frame_0_t *frame = frame_;

	if (frame->produced_live)
		trees_build_VecItem_drop(&frame->produced);
	if (frame->consumed_live)
		trees_mutate_VecTupleVecU8Tree_drop(&frame->consumed);
	if (frame->run_rest_live)
	{
		trees_cursor_ArcStamps_drop(frame->run.stamps);
		trees_chunker_Splitter_drop(&frame->run.splitter);
		trees_build_VecItem_drop(&frame->run.pending);
	}
	if (frame->run_live)
		trees_mutate_Run_drop(&frame->run);
	if (frame->written_live)
		trees_build_Item_drop(&frame->written);
	if (frame->taken_live)
		trees_build_VecItem_drop(&frame->taken);
	if (frame->passed_live)
		std_vec_free_Checked(&frame->passed.f0, 1, 1);
	if (frame->item_live)
		trees_build_Item_drop(&frame->item);
	if (frame->stamps_live)
		trees_cursor_ArcStamps_drop(frame->stamps);
	if (frame->moved_live)
		trees_cursor_Cursor_drop(&frame->moved);
	if (frame->above_live)
		trees_mutate_VecTupleVecU8OptionItem_drop(&frame->above);
}

/*
 * trees_mutate_Level_apply is Level::apply: the level's edits applied,
 * returning the edits they make to the level above in result. A run stops
 * where the next step needs an edit not yet given, unless the edits are all
 * in, and at the root's level the new nodes are kept.
 */
static bool
trees_mutate_Level_apply(TreesMutateLevel *self, HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree,
						 uint32 target, const TreesShape *shape, bool last, StdVec *result, ChunksErrorError *error)
{
	trees_mutate_Level_apply_frame_0_t frame = {.self = self, .io = io, .store = store, .tree = tree,
	.target = target, .shape = shape, .last = last, .result = result, .error = error};

	std_landing(&frame, trees_mutate_Level_apply_body_0, trees_mutate_Level_apply_pad_0);
	return frame.ok;
}


/*
 * trees_mutate_check_run is check_run: an error for a run at a level of a
 * tree whose new nodes' keys don't follow one another in the tree's order,
 * or which don't hold the entries its edits leave, which only a defect of
 * the merge gives.
 */
bool
trees_mutate_check_run(HostIoIo *io, TreesStoreTree tree, uint8 level, const TreesBuildItem *produced,
					   size_t produced_len, int64 expected, const TreesKeyOrder *order, ChunksErrorError *error)
{
	StdWindows	windows = std_slice_windows(produced, produced_len, 2, RUST_FILE, 292, 26);
	const void *window_;
	size_t		window_len;
	uint64		written = 0;

	while (std_slice_Windows_next(&windows, sizeof(TreesBuildItem), &window_, &window_len))
	{
		const TreesBuildItem *pair = window_;
		StdBytes	a;
		StdBytes	b;
		StdOrdering ordering;

		std_index_check(0, window_len, RUST_FILE, 293, 31);
		a = trees_build_Item_key(&pair[0]);
		std_index_check(1, window_len, RUST_FILE, 293, 46);
		b = trees_build_Item_key(&pair[1]);
		if (!trees_KeyOrder_compare(order, io, a.ptr, a.len, b.ptr, b.len, &ordering, error))
			return false;
		if (ordering != STD_ORDERING_LESS)
		{
			static const char text[] = "a merge wrote a tree node whose keys don't follow the node before it";

			*error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
			return false;
		}
	}
	for (size_t k = 0, k_len = produced_len; k < k_len; k++)
		written = std_add_u64(written, trees_build_Item_weight(&produced[k]), STD_ACCUM_FILE, 206, 1);
	if ((int64) written != expected)
	{
		ChunksAddressAddress address = tree.address;
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("a merge into tree "), STD_FMT_ARG(0), STD_FMT_LIT(" wrote "),
			STD_FMT_ARG(1), STD_FMT_LIT(" entries at level "), STD_FMT_ARG(2),
		STD_FMT_LIT(" where its edits leave "), STD_FMT_ARG(3)};
		StdFmtArg	args[] = {{&address, chunks_address_Address_Display_fmt}, {&written, std_u64_Display_fmt},
		{&level, std_u8_Display_fmt}, {&expected, std_i64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	return true;
}

/* trees_mutate_run_edits_frame_0_t is run_edits' state across its pad: the edit being made and pushed, while live. */
typedef struct trees_mutate_run_edits_frame_0_t
{
	HostIoIo   *io;
	const trees_TupleVecU8Tree_t *consumed;
	size_t		consumed_len;
	const TreesBuildItem *produced;
	size_t		produced_len;
	const TreesKeyOrder *order;
	StdVec	   *above;
	ChunksErrorError *error;
	bool		ok;
	trees_TupleVecU8OptionItem_t edit;
	bool		edit_key_live;
	bool		edit_item_live;
} trees_mutate_run_edits_frame_0_t;

/* trees_mutate_run_edits_body_0 is run_edits' body: nothing for a run that wrote the old nodes again, else old and new nodes merged by key into deletions and new items. */
static void
trees_mutate_run_edits_body_0(void *frame_)
{
	trees_mutate_run_edits_frame_0_t *frame = frame_;
	const trees_TupleVecU8Tree_t *consumed = frame->consumed;
	const TreesBuildItem *produced = frame->produced;
	size_t		consumed_len = frame->consumed_len;
	size_t		produced_len = frame->produced_len;
	bool		unchanged = consumed_len == produced_len;
	size_t		gone = 0;
	size_t		new = 0;

	if (unchanged)
		for (size_t k = 0, k_len = consumed_len; k < k_len; k++)
			if (produced[k].tag != TREES_BUILD_ITEM_CHILD ||
				!trees_store_Tree_PartialEq_eq(&produced[k].u.child.tree, &consumed[k].f1))
			{
				unchanged = false;
				break;
			}
	if (unchanged)
	{
		frame->ok = true;
		return;
	}
	for (;;)
	{
		StdOrdering next;

		if (gone == consumed_len && new == produced_len)
		{
			frame->ok = true;
			return;
		}
		if (new == produced_len)
			next = STD_ORDERING_LESS;
		else if (gone == consumed_len)
			next = STD_ORDERING_GREATER;
		else
		{
			StdBytes	key = trees_build_Item_key(&produced[new]);

			if (!trees_KeyOrder_compare(frame->order, frame->io, consumed[gone].f0.ptr, consumed[gone].f0.len, key.ptr,
										key.len, &next, frame->error))
			{
				frame->ok = false;
				return;
			}
		}
		if (next != STD_ORDERING_GREATER)
		{
			const trees_TupleVecU8Tree_t *taken;

			if (gone == consumed_len)
				std_option_unwrap_failed(RUST_FILE, 331, 40);
			taken = &consumed[gone];
			gone = std_wrapping_add_usize(gone, 1);
			if (next == STD_ORDERING_LESS)
			{
				std_vec_clone(&frame->edit.f0, &taken->f0, 1, 1);
				frame->edit_key_live = true;
				frame->edit.has_f1 = false;
				std_vec_push_Checked(frame->above, &frame->edit, sizeof(frame->edit),
									 _Alignof(trees_TupleVecU8OptionItem_t));
				frame->edit_key_live = false;
			}
		}
		if (next != STD_ORDERING_LESS)
		{
			const TreesBuildItem *item;
			StdBytes	key;

			if (new == produced_len)
				std_option_unwrap_failed(RUST_FILE, 337, 35);
			item = &produced[new];
			new = std_wrapping_add_usize(new, 1);
			key = trees_build_Item_key(item);
			std_slice_to_vec_Checked(&frame->edit.f0, key.ptr, key.len, 1, 1);
			frame->edit_key_live = true;
			trees_build_Item_Clone_clone(item, &frame->edit.f1);
			frame->edit.has_f1 = true;
			frame->edit_item_live = true;
			std_vec_push_Checked(frame->above, &frame->edit, sizeof(frame->edit),
								 _Alignof(trees_TupleVecU8OptionItem_t));
			frame->edit_item_live = false;
			frame->edit_key_live = false;
		}
	}
}

/* trees_mutate_run_edits_pad_0 is run_edits' pad: the edit being made, while live. */
static void
trees_mutate_run_edits_pad_0(void *frame_)
{
	trees_mutate_run_edits_frame_0_t *frame = frame_;

	if (frame->edit_item_live)
		trees_build_Item_drop(&frame->edit.f1);
	if (frame->edit_key_live)
		std_vec_free_Checked(&frame->edit.f0, 1, 1);
}

/*
 * trees_mutate_run_edits is run_edits: the edits a run makes to the level
 * above pushed to above, none when it wrote the old nodes again, and
 * otherwise a deletion of each old node's key and the new node for each new
 * node's key.
 */
bool
trees_mutate_run_edits(HostIoIo *io, const trees_TupleVecU8Tree_t *consumed, size_t consumed_len,
					   const TreesBuildItem *produced, size_t produced_len, const TreesKeyOrder *order, StdVec *above,
					   ChunksErrorError *error)
{
	trees_mutate_run_edits_frame_0_t frame = {.io = io, .consumed = consumed, .consumed_len = consumed_len,
		.produced = produced, .produced_len = produced_len, .order = order, .above = above,
	.error = error};

	std_landing(&frame, trees_mutate_run_edits_body_0, trees_mutate_run_edits_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_TREES

/*
 * TREES_MUTATE_SKIP_OLD is SKIP_OLD: whether a merger passes the next old
 * item without taking it, the bug a run's counts catch; its BackendLocal
 * header and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	StdAtomicBool process;
}			TREES_MUTATE_SKIP_OLD = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* trees_mutate_skip_old is skip_old in a test build: SKIP_OLD's value, swapped for false. */
static bool
trees_mutate_skip_old(void)
{
	HostLocalLocal local = host_local_BackendLocal_get(&TREES_MUTATE_SKIP_OLD.local, &STD_ATOMIC_BOOL_TYPE);
	bool		skip;

	skip = std_AtomicBool_swap_relaxed((StdAtomicBool *) host_local_Local_Deref_deref(&local, &STD_ATOMIC_BOOL_TYPE),
									   false);
	host_local_Local_drop(&local, &STD_ATOMIC_BOOL_TYPE);
	return skip;
}

#else

/* trees_mutate_skip_old is skip_old outside a test build: never. */
static bool
trees_mutate_skip_old(void)
{
	return false;
}

#endif							/* DOLTGRES_TEST_TREES */

/* trees_mutate_old_weight is old_weight: how many leaf entries the item a cursor at the level is at stands for. */
static uint64
trees_mutate_old_weight(const TreesCursorCursor *cursor, uint8 level)
{
	TreesNodeNode node;

	if (level == 0)
		return 1;
	node = trees_cursor_Cursor_node(cursor);
	return trees_node_Node_subtree(&node, trees_cursor_Cursor_index(cursor));
}

/* trees_mutate_old_item_frame_0_t is old_item's state across its pad: an internal entry's summary, while live. */
typedef struct trees_mutate_old_item_frame_0_t
{
	TreesCursorCursor *cursor;
	uint8		level;
	StdArcInner *const *stamps;
	TreesBuildItem *result;
	ChunksErrorError *error;
	bool		ok;
	bool		summary_live;
} trees_mutate_old_item_frame_0_t;

/* trees_mutate_old_item_body_0 is old_item's body: a leaf's entry as an old item with the node's stamps, or an internal one's child with its summary and subtree's highest tuple id. */
static void
trees_mutate_old_item_body_0(void *frame_)
{
	trees_mutate_old_item_frame_0_t *frame = frame_;
	TreesCursorCursor *cursor = frame->cursor;
	TreesBuildItem *result = frame->result;
	TreesNodeNode node;
	size_t		i;
	StdBytes	summary;
	StdBytes	key;
	uint64		max_tuple;

	if (frame->level == 0)
	{
		result->tag = TREES_BUILD_ITEM_OLD;
		result->u.old.node = trees_cursor_Cursor_shared(cursor);
		result->u.old.index = trees_cursor_Cursor_index(cursor);
		result->u.old.stamps = std_arc_clone(*frame->stamps);
		frame->ok = true;
		return;
	}
	node = trees_cursor_Cursor_node(cursor);
	i = trees_cursor_Cursor_index(cursor);
	summary = trees_node_Node_summary(&node, i);
	result->u.child.has_summary = summary.ptr != NULL;
	if (result->u.child.has_summary)
	{
		std_slice_to_vec_Checked(&result->u.child.summary, summary.ptr, summary.len, 1, 1);
		frame->summary_live = true;
	}
	if (!trees_subtree_max(std_arc_Deref_deref(*frame->stamps, _Alignof(ChunksTrailerStamps)), i, &max_tuple,
						   frame->error))
	{
		if (frame->summary_live)
		{
			frame->summary_live = false;
			std_vec_free_Checked(&result->u.child.summary, 1, 1);
		}
		frame->ok = false;
		return;
	}
	key = trees_node_Node_key(&node, i);
	std_slice_to_vec_Checked(&result->u.child.key, key.ptr, key.len, 1, 1);
	frame->summary_live = false;
	result->tag = TREES_BUILD_ITEM_CHILD;
	result->u.child.tree = trees_cursor_Cursor_child_tree(cursor);
	result->u.child.count = trees_node_Node_subtree(&node, i);
	result->u.child.max_tuple = max_tuple;
	frame->ok = true;
}

/* trees_mutate_old_item_pad_0 is old_item's pad: the summary, while live. */
static void
trees_mutate_old_item_pad_0(void *frame_)
{
	trees_mutate_old_item_frame_0_t *frame = frame_;

	if (frame->summary_live)
		std_vec_free_Checked(&frame->result->u.child.summary, 1, 1);
}

/* trees_mutate_old_item is old_item: the entry a cursor at the level is at, with its node's stamps. */
static bool
trees_mutate_old_item(TreesCursorCursor *cursor, uint8 level, StdArcInner *const *stamps, TreesBuildItem *result,
					  ChunksErrorError *error)
{
	trees_mutate_old_item_frame_0_t frame = {.cursor = cursor, .level = level, .stamps = stamps, .result = result,
	.error = error};

	std_landing(&frame, trees_mutate_old_item_body_0, trees_mutate_old_item_pad_0);
	return frame.ok;
}

/* trees_mutate_node_stamps is node_stamps: the stamps of a cursor's node, a leaf's row stamps or an internal node's subtrees' highest tuple ids. */
static bool
trees_mutate_node_stamps(HostIoIo *io, TreesStoreNodeStore store, const TreesCursorCursor *cursor,
						 StdArcInner **result, ChunksErrorError *error)
{
	TreesStoreTree tree;

	if (!trees_cursor_Cursor_is_valid(cursor))
	{
		ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();

		*result = std_arc_new(&stamps, sizeof(stamps), _Alignof(ChunksTrailerStamps));
		return true;
	}
	tree = trees_cursor_Cursor_node_tree(cursor);
	return trees_store_cached_stamps(io, store, TREES_STORE_TREE_NAMED(&tree), result, error);
}

/* trees_mutate_new_root_frame_0_t is new_root's state across its pad: the nodes it takes, the builder, and the root's read, while live. */
typedef struct trees_mutate_new_root_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	uint32		target;
	uint8		level;
	StdVec	   *nodes;
	bool		nodes_live;
	size_t		nodes_at;
	const TreesShape *shape;
	TreesStoreTree *result;
	ChunksErrorError *error;
	bool		ok;
	TreesBuildBuilder builder;
	bool		builder_live;
	TreesNodeFlat read;
	bool		read_live;
} trees_mutate_new_root_frame_0_t;

/* trees_mutate_VecItem_IntoIter_drop is a Vec<Item>'s IntoIter's drop glue from an index: the items left, then the buffer. */
static void
trees_mutate_VecItem_IntoIter_drop(StdVec *self, size_t at)
{
	for (size_t k = 0, k_len = std_wrapping_sub_usize(self->len, at); k < k_len; k++)
		trees_build_Item_drop(&STD_VEC_AT(self, TreesBuildItem, std_wrapping_add_usize(at, k)));
	std_vec_free_Checked(self, sizeof(TreesBuildItem), _Alignof(TreesBuildItem));
}

/* trees_mutate_new_root_body_0 is new_root's body. */
static void
trees_mutate_new_root_body_0(void *frame_)
{
	trees_mutate_new_root_frame_0_t *frame = frame_;
	TreesStoreTree tree;

	if (frame->nodes->len == 0)
	{
		trees_build_Builder_shaped(frame->store, frame->target, frame->shape, &frame->builder);
		frame->ok = trees_build_Builder_finish_tree(&frame->builder, frame->io, frame->result, frame->error);
		frame->nodes_live = false;
		trees_build_VecItem_drop(frame->nodes);
		return;
	}
	if (frame->nodes->len == 1)
	{
		const TreesBuildItem *only;

		std_index_check(0, frame->nodes->len, RUST_FILE, 400, 26);
		only = &STD_VEC_AT(frame->nodes, TreesBuildItem, 0);
		if (only->tag != TREES_BUILD_ITEM_CHILD)
		{
			static const char text[] = "internal error: entered unreachable code: a level above the leaves makes internal items";

			std_panic(RUST_FILE, 402, 49, text, sizeof(text) - 1);
		}
		tree = only->u.child.tree;
	}
	else
	{
		uint8		above = STD_ADD(u8, frame->level, 1, 405, 64);

		trees_build_Builder_at_level(frame->store, frame->target, above, frame->shape, &frame->builder);
		frame->builder_live = true;
		for (frame->nodes_at = 0; frame->nodes_at < frame->nodes->len;)
		{
			TreesBuildItem node = STD_VEC_AT(frame->nodes, TreesBuildItem, frame->nodes_at);
			size_t		at = STD_ADD(usize, (size_t) frame->level, 1, 407, 34);

			frame->nodes_at = std_wrapping_add_usize(frame->nodes_at, 1);
			if (!trees_build_Builder_push(&frame->builder, frame->io, at, &node, frame->error))
			{
				frame->nodes_live = false;
				trees_mutate_VecItem_IntoIter_drop(frame->nodes, frame->nodes_at);
				frame->builder_live = false;
				trees_build_Builder_drop(&frame->builder);
				frame->ok = false;
				return;
			}
		}
		frame->nodes_live = false;
		trees_mutate_VecItem_IntoIter_drop(frame->nodes, frame->nodes_at);
		frame->builder_live = false;
		frame->ok = trees_build_Builder_finish_tree(&frame->builder, frame->io, frame->result, frame->error);
		return;
	}
	for (;;)
	{
		TreesNodeNode node;

		if (!frame->store.ops->read(frame->store.self, frame->io, &tree.address, tree.locator, &frame->read,
									frame->error))
		{
			frame->nodes_live = false;
			trees_build_VecItem_drop(frame->nodes);
			frame->ok = false;
			return;
		}
		frame->read_live = true;
		node = trees_node_Flat_node(&frame->read);
		if (trees_node_Node_is_leaf(&node) || trees_node_Node_len(&node) != 1)
		{
			frame->read_live = false;
			trees_node_Flat_drop(&frame->read);
			frame->nodes_live = false;
			trees_build_VecItem_drop(frame->nodes);
			*frame->result = tree;
			frame->ok = true;
			return;
		}
		tree = trees_node_Node_child_tree(&node, 0);
		if (trees_node_Node_level(&node) == 1)
		{
			frame->read_live = false;
			trees_node_Flat_drop(&frame->read);
			frame->nodes_live = false;
			trees_build_VecItem_drop(frame->nodes);
			*frame->result = tree;
			frame->ok = true;
			return;
		}
		frame->read_live = false;
		trees_node_Flat_drop(&frame->read);
	}
}

/* trees_mutate_new_root_pad_0 is new_root's pad: the read, the builder, and the nodes left, while live. */
static void
trees_mutate_new_root_pad_0(void *frame_)
{
	trees_mutate_new_root_frame_0_t *frame = frame_;

	if (frame->read_live)
		trees_node_Flat_drop(&frame->read);
	if (frame->builder_live)
		trees_build_Builder_drop(&frame->builder);
	if (frame->nodes_live)
		trees_mutate_VecItem_IntoIter_drop(frame->nodes, frame->nodes_at);
}

/*
 * trees_mutate_new_root is new_root: the tree whose root level, at the
 * level, now has the nodes, reading no leaf; it takes the nodes.
 */
static bool
trees_mutate_new_root(HostIoIo *io, TreesStoreNodeStore store, uint32 target, uint8 level, StdVec *nodes,
					  const TreesShape *shape, TreesStoreTree *result, ChunksErrorError *error)
{
	trees_mutate_new_root_frame_0_t frame = {.io = io, .store = store, .target = target, .level = level,
	.nodes = nodes, .nodes_live = true, .shape = shape, .result = result, .error = error};

	std_landing(&frame, trees_mutate_new_root_body_0, trees_mutate_new_root_pad_0);
	return frame.ok;
}


#ifdef DOLTGRES_TEST_TREES

/* trees_mutate_tests_PhantomDataLoose_Debug_fmt is PhantomData<Loose>'s Debug: PhantomData and its type's name. */
static bool
trees_mutate_tests_PhantomDataLoose_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char text[] = "PhantomData<trees::store::Loose>";

	return std_fmt_write_str(f, text, sizeof(text) - 1);
}

/* trees_mutate_tests_FreshLoose_Debug_fmt is Fresh<Loose>'s derived Debug: its tree and its PhantomData brand. */
static bool
trees_mutate_tests_FreshLoose_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesStoreFresh *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Fresh", 5);

	std_DebugStruct_field(&s, "tree", 4, &self->tree, trees_store_TreeRef_Debug_fmt);
	std_DebugStruct_field(&s, "brand", 5, NULL, trees_mutate_tests_PhantomDataLoose_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_closure_0 is the test's key, |n: u32| format!("key{n:06}").into_bytes(). */
static void
test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_closure_0(uint32 n,
																									StdVec *result)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("key"), {NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 6}}};
	StdFmtArg	args[] = {{&n, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/*
 * test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_frame_0_t
 * is the test's state across its pad: an unwrap's error, the merge's error
 * and its text, the edits and their clone, the builder, the store, and the
 * test Io, while live.
 */
typedef struct test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	TreesStoreMemoryStore store;
	bool		store_live;
	TreesBuildBuilder builder;
	bool		builder_live;
	StdVec		key;
	bool		key_live;
	TreesMutateEdit edit;
	bool		edit_live;
	StdVec		edits;
	bool		edits_live;
	StdVec		edits_clone;
	bool		edits_clone_live;
	TreesShape	shape;
	bool		shape_live;
	ChunksErrorError err;
	bool		err_live;
	StdString text;
	bool		text_live;
	ChunksErrorError error;
	bool		error_live;
} test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_frame_0_t;

/* test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_body_0 is the test's body. */
static void
test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_body_0(void *frame_)
{
	test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_frame_0_t *frame =
		frame_;
	HostIoIo   *io;
	TreesStoreFresh fresh;
	TreesStoreTreeRef tree;
	static const char needle[] = "where its edits leave";

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	frame->store = trees_store_MemoryStore_new();
	frame->store_live = true;
	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(&frame->store), 128, &frame->builder);
	frame->builder_live = true;
	for (uint32 n = 0; n < 2000; n = std_forward_unchecked_u32_Checked(n, 1))
	{
		StdVec		value;
		TreesBuildEntry entry;

		test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_closure_0(n, &frame->key);
		frame->key_live = true;
		std_slice_to_vec_Checked(&value, "value", 5, 1, 1);
		frame->key_live = false;
		trees_build_Entry_new_VecU8VecU8(&frame->key, &value, &entry);
		if (!trees_build_Builder_add(&frame->builder, io, &entry, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(438, 68, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
	}
	frame->builder_live = false;
	if (!trees_build_Builder_finish(&frame->builder, io, &TREES_STORE_MEMORY_STORE_BRANDED_OPS, &fresh, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(440, 39, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	tree = trees_store_Fresh_as_ref(&fresh);
	{
		StdVec		value;
		TreesMutateEdit *boxed;

		test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_closure_0(1000, &frame->key);
		frame->key_live = true;
		std_slice_to_vec_Checked(&value, "changed", 7, 1, 1);
		frame->key_live = false;
		frame->edit.tag = TREES_MUTATE_EDIT_PUT;
		trees_build_Entry_new_VecU8VecU8(&frame->key, &value, &frame->edit.u.put);
		frame->edit_live = true;
		boxed = std_box_new_uninit_layout_Checked(sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
		STD_CHECK_ALIGNED(boxed, _Alignof(TreesMutateEdit), 441, 21);
		STD_CHECK_NULL_DEREF(boxed, 441, 21);
		boxed[0] = frame->edit;
		frame->edit_live = false;
		std_box_slice_into_vec_Checked(&frame->edits, boxed, 1, sizeof(TreesMutateEdit));
		frame->edits_live = true;
	}
	std_slice_to_vec_clone(&frame->edits_clone, frame->edits.ptr, frame->edits.len, sizeof(TreesMutateEdit),
						   _Alignof(TreesMutateEdit), trees_mutate_Edit_Clone_clone, trees_mutate_Edit_drop);
	frame->edits_clone_live = true;
	frame->shape = trees_Shape_Default_default();
	frame->shape_live = true;
	frame->edits_clone_live = false;
	if (!trees_mutate_mutate(io, TREES_STORE_MEMORY_STORE_BRANDED(&frame->store),
							 TREES_STORE_TREE_REF_NAMED(&tree), 128, &frame->edits_clone,
							 &frame->shape, &fresh, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(442, 85, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->shape_live = false;
	trees_Shape_drop(&frame->shape);
	{
		HostLocalLocal local = host_local_BackendLocal_get(&TREES_MUTATE_SKIP_OLD.local, &STD_ATOMIC_BOOL_TYPE);

		std_AtomicBool_store_relaxed((StdAtomicBool *) host_local_Local_Deref_deref(&local, &STD_ATOMIC_BOOL_TYPE),
									 true);
		host_local_Local_drop(&local, &STD_ATOMIC_BOOL_TYPE);
	}
	frame->shape = trees_Shape_Default_default();
	frame->shape_live = true;
	frame->edits_live = false;
	if (trees_mutate_mutate(io, TREES_STORE_MEMORY_STORE_BRANDED(&frame->store),
							TREES_STORE_TREE_REF_NAMED(&tree), 128, &frame->edits,
							&frame->shape, &fresh, &frame->err))
		STD_RESULT_EXPECT_FAILED(444, 79, "called `Result::unwrap_err()` on an `Ok` value", &fresh,
								 trees_mutate_tests_FreshLoose_Debug_fmt);
	frame->err_live = true;
	frame->shape_live = false;
	trees_Shape_drop(&frame->shape);
	std_to_string(&frame->err, chunks_error_Error_Display_fmt, &frame->text);
	frame->text_live = true;
	if (!std_str_contains_str_Checked(frame->text.ptr, frame->text.len, needle, sizeof(needle) - 1))
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->err, chunks_error_Error_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 445, 9, &arguments);
	}
	frame->text_live = false;
	std_String_drop(&frame->text);
	frame->err_live = false;
	chunks_error_Error_drop(&frame->err);
	frame->store_live = false;
	trees_store_MemoryStore_drop(&frame->store);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_pad_0 is the test's pad: an unwrap's error, then every other value its frame holds, in reverse order. */
static void
test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_pad_0(void *frame_)
{
	test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_frame_0_t *frame =
		frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->text_live)
		std_String_drop(&frame->text);
	if (frame->err_live)
		chunks_error_Error_drop(&frame->err);
	if (frame->shape_live)
		trees_Shape_drop(&frame->shape);
	if (frame->edits_clone_live)
		trees_mutate_VecEdit_drop(&frame->edits_clone);
	if (frame->edits_live)
		trees_mutate_VecEdit_drop(&frame->edits);
	if (frame->edit_live)
		trees_mutate_Edit_drop(&frame->edit);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->builder_live)
		trees_build_Builder_drop(&frame->builder);
	if (frame->store_live)
		trees_store_MemoryStore_drop(&frame->store);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree
 * is the test: a merge that passes an old entry without taking it fails,
 * the run's counts catching it, instead of writing the tree.
 */
void
test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree(void)
{
	test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_frame_0_t frame = {0};

	std_landing(&frame, test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_body_0,
				test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree_pad_0);
}

#endif							/* DOLTGRES_TEST_TREES */
