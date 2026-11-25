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
 * Cursors (trees' cursor.rs): a position at one level of a tree, which
 * descends from the root by a choice of child at each node, moves along its
 * level by climbing past the ends of nodes and descending again, reads its
 * own node within the entry that made or resumed it, and is kept across
 * calls as a KeptCursor; and the row stamps a walk keeps of the leaf it last
 * read.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/cursor.h"

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "cursor.rs"

/* trees_cursor_ArcSliceU8_drop is Arc<[u8]>'s drop glue: its strong count down, and its block freed at zero. */
void
trees_cursor_ArcSliceU8_drop(StdArcSlice *self)
{
	std_arc_drop(self->ptr, self->len, 1, NULL);
}

/* The OnceCell<Arc<[u8]>> instance's definitions. */
#define STD_ONCE_CELL_SCOPE extern
#define STD_ONCE_CELL_DEFINE
#define STD_ONCE_CELL_TS ArcSliceU8
#define STD_ONCE_CELL_T StdArcSlice
#define STD_ONCE_CELL_DROP trees_cursor_ArcSliceU8_drop
#include "doltgres/std/once_cell_template.h"

/* trees_cursor_ArcStamps_drop is Arc<Stamps>'s drop glue: its strong count down, and its stamps dropped at zero. */
void
trees_cursor_ArcStamps_drop(StdArcInner *self)
{
	std_arc_drop(self, sizeof(ChunksTrailerStamps), _Alignof(ChunksTrailerStamps), chunks_trailer_Stamps_drop);
}

/* trees_cursor_Frame_of returns the frame of a node at a position. */
static TreesCursorFrame
trees_cursor_Frame_of(TreesNodeNode node, TreesStoreTree tree, size_t idx)
{
	TreesCursorFrame result;

	result.tree = tree;
	result.idx = idx;
	result.len = trees_node_Node_len(&node);
	result.level = trees_node_Node_level(&node);
	return result;
}

/* trees_cursor_Stay_drop is Stay's drop glue: its kept pin or its bytes. */
static void
trees_cursor_Stay_drop(TreesCursorStay *self)
{
	switch (self->tag)
	{
		case TREES_CURSOR_STAY_PINNED:
			host_cache_Kept_drop(&self->u.pinned);
			break;
		case TREES_CURSOR_STAY_OWN:
			std_vec_free_Checked(&self->u.own, 1, 1);
			break;
	}
}

/* trees_cursor_KeptCursor_drop is KeptCursor's drop glue: its path, then its node. */
void
trees_cursor_KeptCursor_drop(TreesCursorKeptCursor *self)
{
	std_vec_free_Checked(&self->path, sizeof(TreesCursorFrame), _Alignof(TreesCursorFrame));
	if (self->has_node)
		trees_cursor_Stay_drop(&self->node);
}

/* trees_cursor_Cursor_drop is Cursor's drop glue: its path, its node, then its copy. */
void
trees_cursor_Cursor_drop(TreesCursorCursor *self)
{
	std_vec_free_Checked(&self->path, sizeof(TreesCursorFrame), _Alignof(TreesCursorFrame));
	if (self->has_node)
		trees_node_Flat_drop(&self->node);
	std_OnceCell_drop_ArcSliceU8(&self->copy);
}

/*
 * trees_cursor_KeptCursor_resume_frame_0_t is resume's state across its pad:
 * the kept cursor it takes, whose fields it moves out, and its result.
 */
typedef struct trees_cursor_KeptCursor_resume_frame_0_t
{
	TreesCursorKeptCursor self;
	bool		path_live;
	bool		kept_live;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor *result;
	ChunksErrorError *error;
	bool		ok;
} trees_cursor_KeptCursor_resume_frame_0_t;

/* trees_cursor_KeptCursor_resume_body_0 is resume's body. */
static void
trees_cursor_KeptCursor_resume_body_0(void *frame_)
{
	trees_cursor_KeptCursor_resume_frame_0_t *frame = frame_;
	bool		has_node = false;
	TreesNodeFlat node;

	if (frame->self.has_node)
	{
		switch (frame->self.node.tag)
		{
			case TREES_CURSOR_STAY_OWN:
				node.f0.tag = HOST_CACHE_READ_UNCACHED;
				node.f0.u.uncached = frame->self.node.u.own;
				has_node = true;
				break;
			case TREES_CURSOR_STAY_PINNED:
				{
					HostCachePinned pinned;
					bool		resumed;

					frame->kept_live = false;
					resumed = host_cache_Kept_resume(&frame->self.node.u.pinned, frame->io, &pinned);
					if (resumed)
					{
						node.f0.tag = HOST_CACHE_READ_CACHED;
						node.f0.u.cached = pinned;
					}
					else if (frame->self.path.len > 0)
					{
						const TreesCursorFrame *last = &STD_VEC_AT(&frame->self.path, TreesCursorFrame,
																   std_wrapping_sub_usize(frame->self.path.len, 1));
						TreesStoreTree tree = last->tree;

						if (!trees_pin_Pin_own(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&tree), &node,
											   frame->error))
						{
							frame->path_live = false;
							std_vec_free_Checked(&frame->self.path, sizeof(TreesCursorFrame),
												 _Alignof(TreesCursorFrame));
							frame->ok = false;
							return;
						}
					}
					else
					{
						StdString	message;

						std_string_from_Checked(&message, "a kept cursor has a node and no path", 36);
						*frame->error = chunks_error_Error_corrupt(&message);
						frame->path_live = false;
						std_vec_free_Checked(&frame->self.path, sizeof(TreesCursorFrame),
											 _Alignof(TreesCursorFrame));
						frame->ok = false;
						return;
					}
					has_node = true;
				}
				break;
		}
	}
	frame->path_live = false;
	frame->result->path = frame->self.path;
	frame->result->has_node = has_node;
	if (has_node)
		frame->result->node = node;
	std_OnceCell_new_ArcSliceU8(&frame->result->copy);
	frame->result->valid = frame->self.valid;
	frame->result->read_ahead = frame->self.read_ahead;
	frame->ok = true;
}

/* trees_cursor_KeptCursor_resume_pad_0 is resume's pad: the kept pin and the path, while it holds them. */
static void
trees_cursor_KeptCursor_resume_pad_0(void *frame_)
{
	trees_cursor_KeptCursor_resume_frame_0_t *frame = frame_;

	if (frame->kept_live)
		host_cache_Kept_drop(&frame->self.node.u.pinned);
	if (frame->path_live)
		std_vec_free_Checked(&frame->self.path, sizeof(TreesCursorFrame), _Alignof(TreesCursorFrame));
}

/*
 * trees_cursor_KeptCursor_resume returns the cursor, which it takes, in the
 * running entry, reading its own node again by its name when the pin it
 * kept was released meanwhile.
 */
bool
trees_cursor_KeptCursor_resume(TreesCursorKeptCursor *self, HostIoIo *io, TreesPinPin *pin,
							   TreesCursorCursor *result, ChunksErrorError *error)
{
	trees_cursor_KeptCursor_resume_frame_0_t frame = {.self = *self, .path_live = true, .io = io, .pin = pin,
	.result = result, .error = error};

	frame.kept_live = self->has_node && self->node.tag == TREES_CURSOR_STAY_PINNED;
	std_landing(&frame, trees_cursor_KeptCursor_resume_body_0, trees_cursor_KeptCursor_resume_pad_0);
	return frame.ok;
}

/* trees_cursor_Cursor_keep returns the cursor, which it takes, in the form that lives across calls. */
void
trees_cursor_Cursor_keep(TreesCursorCursor *self, TreesCursorKeptCursor *result)
{
	result->has_node = self->has_node;
	if (self->has_node)
	{
		switch (self->node.f0.tag)
		{
			case HOST_CACHE_READ_CACHED:
				result->node.tag = TREES_CURSOR_STAY_PINNED;
				result->node.u.pinned = host_cache_Pinned_keep(&self->node.f0.u.cached);
				break;
			case HOST_CACHE_READ_UNCACHED:
				result->node.tag = TREES_CURSOR_STAY_OWN;
				result->node.u.own = self->node.f0.u.uncached;
				break;
		}
	}
	result->path = self->path;
	result->valid = self->valid;
	result->read_ahead = self->read_ahead;
	std_OnceCell_drop_ArcSliceU8(&self->copy);
}

/* trees_cursor_Cursor_start returns a cursor at the root of a tree, before it descends. */
static bool
trees_cursor_Cursor_start(HostIoIo *io, TreesPinPin *pin, TreesStoreTree tree, TreesCursorCursor *result,
						  ChunksErrorError *error)
{
	TreesNodeNode node;
	TreesCursorFrame *boxed;
	TreesCursorFrame frame;

	if (!trees_pin_Pin_node(pin, io, TREES_STORE_TREE_NAMED(&tree), &node, error))
		return false;
	frame = trees_cursor_Frame_of(node, tree, 0);
	boxed = std_box_new_uninit_layout_Checked(sizeof(TreesCursorFrame), _Alignof(TreesCursorFrame));
	STD_CHECK_ALIGNED(boxed, _Alignof(TreesCursorFrame), 111, 20);
	STD_CHECK_NULL_DEREF(boxed, 111, 20);
	boxed[0] = frame;
	std_box_slice_into_vec_Checked(&result->path, boxed, 1, sizeof(TreesCursorFrame));
	result->has_node = false;
	std_OnceCell_new_ArcSliceU8(&result->copy);
	result->valid = false;
	result->read_ahead = TREES_CURSOR_DEFAULT_READ_AHEAD;
	return true;
}

/*
 * TreesCursorChoose is descend's choice of child or of position in a node, a
 * borrowed &dyn Fn(&mut Io, Node) -> Result<usize>: its function and its
 * captures.
 */
typedef bool (*TreesCursorChoose) (void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
								   ChunksErrorError *error);

/* trees_cursor_Cursor_descend_frame_0_t is descend's state across its pad: its arguments, and the new read of an assignment, while live. */
typedef struct trees_cursor_Cursor_descend_frame_0_t
{
	TreesCursorCursor *self;
	HostIoIo   *io;
	TreesPinPin *pin;
	uint8		level;
	TreesCursorChoose choose;
	void	   *choose_captures;
	TreesCursorChoose place;
	void	   *place_captures;
	bool		chosen;
	TreesNodeFlat flat;
	bool		flat_live;
	ChunksErrorError *error;
	bool		ok;
} trees_cursor_Cursor_descend_frame_0_t;

/* trees_cursor_Cursor_descend_body_0 is descend's body. */
static void
trees_cursor_Cursor_descend_body_0(void *frame_)
{
	trees_cursor_Cursor_descend_frame_0_t *frame = frame_;

	for (;;)
	{
		size_t		last = STD_SUB(usize, frame->self->path.len, 1, 128, 24);
		TreesCursorFrame cursor_frame;
		TreesNodeNode node;
		size_t		idx;
		TreesStoreTree child;
		TreesNodeNode child_node;
		TreesCursorFrame pushed;

		std_index_check(last, frame->self->path.len, RUST_FILE, 129, 34);
		cursor_frame = STD_VEC_AT(&frame->self->path, TreesCursorFrame, last);
		if (!trees_pin_Pin_node(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&cursor_frame.tree), &node, frame->error))
		{
			frame->ok = false;
			return;
		}
		if (cursor_frame.level == frame->level)
		{
			bool		read;

			if (!frame->chosen)
			{
				size_t		placed;

				if (!frame->place(frame->place_captures, frame->io, node, &placed, frame->error))
				{
					frame->ok = false;
					return;
				}
				std_index_check(last, frame->self->path.len, RUST_FILE, 133, 30);
				STD_VEC_AT(&frame->self->path, TreesCursorFrame, last).idx = placed;
			}
			std_index_check(last, frame->self->path.len, RUST_FILE, 135, 39);
			frame->self->valid = STD_VEC_AT(&frame->self->path, TreesCursorFrame, last).idx < cursor_frame.len;
			if (!frame->self->has_node)
				read = true;
			else
			{
				TreesNodeNode own = trees_node_Flat_node(&frame->self->node);
				ChunksAddressAddress address = trees_node_Node_address(&own);

				read = !chunks_address_Address_PartialEq_eq(&address, &cursor_frame.tree.address);
			}
			if (read)
			{
				if (!trees_pin_Pin_own(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&cursor_frame.tree), &frame->flat,
									   frame->error))
				{
					frame->ok = false;
					return;
				}
				frame->flat_live = true;
				if (frame->self->has_node)
					trees_node_Flat_drop(&frame->self->node);
				frame->flat_live = false;
				frame->self->node = frame->flat;
				frame->self->has_node = true;
				std_OnceCell_drop_ArcSliceU8(&frame->self->copy);
				std_OnceCell_new_ArcSliceU8(&frame->self->copy);
			}
			frame->ok = true;
			return;
		}
		if (!frame->chosen)
		{
			size_t		chose;

			if (!frame->choose(frame->choose_captures, frame->io, node, &chose, frame->error))
			{
				frame->ok = false;
				return;
			}
			std_index_check(last, frame->self->path.len, RUST_FILE, 143, 26);
			STD_VEC_AT(&frame->self->path, TreesCursorFrame, last).idx = chose;
		}
		frame->chosen = false;
		std_index_check(last, frame->self->path.len, RUST_FILE, 146, 32);
		idx = STD_VEC_AT(&frame->self->path, TreesCursorFrame, last).idx;
		child = trees_node_Node_child_tree(&node, idx);
		if (!trees_pin_Pin_node(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&child), &child_node, frame->error))
		{
			frame->ok = false;
			return;
		}
		pushed = trees_cursor_Frame_of(child_node, child, 0);
		std_vec_push_Checked(&frame->self->path, &pushed, sizeof(TreesCursorFrame), _Alignof(TreesCursorFrame));
	}
}

/*
 * trees_cursor_Cursor_descend_pad_0 is descend's pad: the new read stored
 * should the old node's drop unwind, as an assignment stores it.
 */
static void
trees_cursor_Cursor_descend_pad_0(void *frame_)
{
	trees_cursor_Cursor_descend_frame_0_t *frame = frame_;

	if (frame->flat_live)
	{
		frame->self->node = frame->flat;
		frame->self->has_node = true;
	}
}

/*
 * trees_cursor_Cursor_descend extends the path from its last node down to
 * the level, choosing each child with the function of the node, and then
 * positions in the level's node with the other function; with chosen set,
 * the last node's position is kept as it is.
 */
static bool
trees_cursor_Cursor_descend(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, uint8 level,
							TreesCursorChoose choose, void *choose_captures, TreesCursorChoose place,
							void *place_captures, bool chosen, ChunksErrorError *error)
{
	trees_cursor_Cursor_descend_frame_0_t frame = {.self = self, .io = io, .pin = pin, .level = level,
		.choose = choose, .choose_captures = choose_captures, .place = place, .place_captures = place_captures,
	.chosen = chosen, .error = error};

	std_landing(&frame, trees_cursor_Cursor_descend_body_0, trees_cursor_Cursor_descend_pad_0);
	return frame.ok;
}

/* TreesCursorCursorSeekToClosure0 is seek_to's search's captures: the key, whether to search past it, and the order. */
typedef struct TreesCursorCursorSeekToClosure0
{
	const uint8 *key;
	size_t		key_len;
	bool		past;
	const TreesKeyOrder *order;
} TreesCursorCursorSeekToClosure0;

/* trees_cursor_Cursor_seek_to_closure_0 is seek_to's search: the key's position in the node, or past it. */
static bool
trees_cursor_Cursor_seek_to_closure_0(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
									  ChunksErrorError *error)
{
	TreesCursorCursorSeekToClosure0 *closure = captures;

	if (closure->past)
		return trees_node_Node_search_past(&node, io, closure->key, closure->key_len, closure->order, result, error);
	return trees_node_Node_search(&node, io, closure->key, closure->key_len, closure->order, result, error);
}

/* trees_cursor_Cursor_seek_to_closure_1 is seek_to's choice of child: the search's position, at most the node's last. */
static bool
trees_cursor_Cursor_seek_to_closure_1(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
									  ChunksErrorError *error)
{
	size_t		found;

	if (!trees_cursor_Cursor_seek_to_closure_0(captures, io, node, &found, error))
		return false;
	*result = std_min_usize(found, std_saturating_sub_usize(trees_node_Node_len(&node), 1));
	return true;
}

/* trees_cursor_Cursor_seek_to_frame_0_t is seek_to's state across its pad: its arguments, its result, and the cursor it builds, while live. */
typedef struct trees_cursor_Cursor_seek_to_frame_0_t
{
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesStoreTree tree;
	TreesCursorCursorSeekToClosure0 search;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TreesCursorCursor *result;
	ChunksErrorError *error;
	bool		ok;
} trees_cursor_Cursor_seek_to_frame_0_t;

/* trees_cursor_Cursor_seek_to_body_0 is seek_to's body. */
static void
trees_cursor_Cursor_seek_to_body_0(void *frame_)
{
	trees_cursor_Cursor_seek_to_frame_0_t *frame = frame_;

	if (!trees_cursor_Cursor_start(frame->io, frame->pin, frame->tree, &frame->cursor, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->cursor_live = true;
	if (!trees_cursor_Cursor_descend(&frame->cursor, frame->io, frame->pin, 0, trees_cursor_Cursor_seek_to_closure_1,
									 &frame->search, trees_cursor_Cursor_seek_to_closure_0, &frame->search, false,
									 frame->error))
	{
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
		frame->ok = false;
		return;
	}
	frame->cursor_live = false;
	*frame->result = frame->cursor;
	frame->ok = true;
}

/* trees_cursor_Cursor_seek_to_pad_0 is seek_to's pad: the cursor, while live. */
static void
trees_cursor_Cursor_seek_to_pad_0(void *frame_)
{
	trees_cursor_Cursor_seek_to_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
}

/* trees_cursor_Cursor_seek_to returns a leaf cursor at the first entry whose key is not below the key, or above it when past. */
static bool
trees_cursor_Cursor_seek_to(HostIoIo *io, TreesPinPin *pin, TreesStoreTree tree, const uint8 *key, size_t key_len,
							bool past, const TreesKeyOrder *order, TreesCursorCursor *result, ChunksErrorError *error)
{
	trees_cursor_Cursor_seek_to_frame_0_t frame = {.io = io, .pin = pin, .tree = tree, .search = {.key = key,
	.key_len = key_len, .past = past, .order = order}, .result = result, .error = error};

	std_landing(&frame, trees_cursor_Cursor_seek_to_body_0, trees_cursor_Cursor_seek_to_pad_0);
	return frame.ok;
}

/* trees_cursor_Cursor_seek returns a leaf cursor at the first entry whose key is not below the key. */
bool
trees_cursor_Cursor_seek(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, const uint8 *key, size_t key_len,
						 const TreesKeyOrder *order, TreesCursorCursor *result, ChunksErrorError *error)
{
	TreesStoreTree tree_2 = trees_store_name(tree);

	return trees_cursor_Cursor_seek_to(io, pin, tree_2, key, key_len, false, order, result, error);
}

/* trees_cursor_Cursor_seek_past returns a leaf cursor at the first entry whose key is above the key, reading no leaf that ends at the key. */
bool
trees_cursor_Cursor_seek_past(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, const uint8 *key,
							  size_t key_len, const TreesKeyOrder *order, TreesCursorCursor *result,
							  ChunksErrorError *error)
{
	TreesStoreTree tree_2 = trees_store_name(tree);

	return trees_cursor_Cursor_seek_to(io, pin, tree_2, key, key_len, true, order, result, error);
}

/* TreesCursorCursorSeekNodeClosure0 is seek_node's choice of child's captures: the key and the order, by reference. */
typedef struct TreesCursorCursorSeekNodeClosure0
{
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
} TreesCursorCursorSeekNodeClosure0;

/* trees_cursor_Cursor_seek_node_closure_0 is seek_node's choice of child: the key's position, at most the node's last. */
static bool
trees_cursor_Cursor_seek_node_closure_0(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
										ChunksErrorError *error)
{
	TreesCursorCursorSeekNodeClosure0 *closure = captures;
	size_t		found;

	if (!trees_node_Node_search(&node, io, closure->key, closure->key_len, closure->order, &found, error))
		return false;
	*result = std_min_usize(found, std_saturating_sub_usize(trees_node_Node_len(&node), 1));
	return true;
}

/* trees_cursor_Cursor_seek_node_closure_1 is seek_node's position in the level's node: its first entry. */
static bool
trees_cursor_Cursor_seek_node_closure_1(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
										ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/* trees_cursor_Cursor_seek_node_frame_0_t is seek_node's state across its pad: its arguments, its result, and the cursor it builds, while live. */
typedef struct trees_cursor_Cursor_seek_node_frame_0_t
{
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesStoreTree tree;
	TreesCursorCursorSeekNodeClosure0 choose;
	uint8		level;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TreesCursorCursor *result;
	ChunksErrorError *error;
	bool		ok;
} trees_cursor_Cursor_seek_node_frame_0_t;

/* trees_cursor_Cursor_seek_node_body_0 is seek_node's body. */
static void
trees_cursor_Cursor_seek_node_body_0(void *frame_)
{
	trees_cursor_Cursor_seek_node_frame_0_t *frame = frame_;
	uint8		level_2;

	if (!trees_cursor_Cursor_start(frame->io, frame->pin, frame->tree, &frame->cursor, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->cursor_live = true;
	std_index_check(0, frame->cursor.path.len, RUST_FILE, 206, 42);
	level_2 = std_min_u8(frame->level, STD_VEC_AT(&frame->cursor.path, TreesCursorFrame, 0).level);
	if (!trees_cursor_Cursor_descend(&frame->cursor, frame->io, frame->pin, level_2, trees_cursor_Cursor_seek_node_closure_0,
									 &frame->choose, trees_cursor_Cursor_seek_node_closure_1, NULL, false, frame->error))
	{
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
		frame->ok = false;
		return;
	}
	frame->cursor_live = false;
	*frame->result = frame->cursor;
	frame->ok = true;
}

/* trees_cursor_Cursor_seek_node_pad_0 is seek_node's pad: the cursor, while live. */
static void
trees_cursor_Cursor_seek_node_pad_0(void *frame_)
{
	trees_cursor_Cursor_seek_node_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
}

/* trees_cursor_Cursor_seek_node returns a cursor at the first entry of the node at the level that holds the key, or would hold it. */
bool
trees_cursor_Cursor_seek_node(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, const uint8 *key,
							  size_t key_len, uint8 level, const TreesKeyOrder *order, TreesCursorCursor *result,
							  ChunksErrorError *error)
{
	TreesStoreTree tree_2 = trees_store_name(tree);
	trees_cursor_Cursor_seek_node_frame_0_t frame = {.io = io, .pin = pin, .tree = tree_2, .choose = {.key = key, .key_len = key_len, .order = order}, .level = level, .result = result, .error = error};

	std_landing(&frame, trees_cursor_Cursor_seek_node_body_0, trees_cursor_Cursor_seek_node_pad_0);
	return frame.ok;
}

/* trees_cursor_Cursor_first returns a leaf cursor at the tree's first entry. */
bool
trees_cursor_Cursor_first(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, TreesCursorCursor *result,
						  ChunksErrorError *error)
{
	TreesStoreTree tree_2 = trees_store_name(tree);

	return trees_cursor_Cursor_first_with_read_ahead(io, pin, TREES_STORE_TREE_NAMED(&tree_2),
													 TREES_CURSOR_DEFAULT_READ_AHEAD, result, error);
}

/* trees_cursor_Cursor_first_with_read_ahead_closure_0 is first_with_read_ahead's choice of child: the first. */
static bool
trees_cursor_Cursor_first_with_read_ahead_closure_0(void *captures, HostIoIo *io, TreesNodeNode node,
													size_t *result, ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/* trees_cursor_Cursor_first_with_read_ahead_closure_1 is first_with_read_ahead's position in the leaf: the first. */
static bool
trees_cursor_Cursor_first_with_read_ahead_closure_1(void *captures, HostIoIo *io, TreesNodeNode node,
													size_t *result, ChunksErrorError *error)
{
	*result = 0;
	return true;
}

static bool trees_cursor_Cursor_read_ahead(const TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin,
										   size_t start, size_t end, ChunksErrorError *error);

/* trees_cursor_Cursor_first_with_read_ahead_frame_0_t is first_with_read_ahead's state across its pad: its arguments, its result, and the cursor it builds, while live. */
typedef struct trees_cursor_Cursor_first_with_read_ahead_frame_0_t
{
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesStoreTree tree;
	size_t		leaves;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TreesCursorCursor *result;
	ChunksErrorError *error;
	bool		ok;
} trees_cursor_Cursor_first_with_read_ahead_frame_0_t;

/* trees_cursor_Cursor_first_with_read_ahead_body_0 is first_with_read_ahead's body. */
static void
trees_cursor_Cursor_first_with_read_ahead_body_0(void *frame_)
{
	trees_cursor_Cursor_first_with_read_ahead_frame_0_t *frame = frame_;

	if (!trees_cursor_Cursor_start(frame->io, frame->pin, frame->tree, &frame->cursor, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->cursor_live = true;
	frame->cursor.read_ahead = frame->leaves;
	if (!trees_cursor_Cursor_descend(&frame->cursor, frame->io, frame->pin, 0, trees_cursor_Cursor_first_with_read_ahead_closure_0,
									 NULL, trees_cursor_Cursor_first_with_read_ahead_closure_1, NULL, false, frame->error) ||
		!trees_cursor_Cursor_read_ahead(&frame->cursor, frame->io, frame->pin, 1, frame->leaves, frame->error))
	{
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
		frame->ok = false;
		return;
	}
	frame->cursor_live = false;
	*frame->result = frame->cursor;
	frame->ok = true;
}

/* trees_cursor_Cursor_first_with_read_ahead_pad_0 is first_with_read_ahead's pad: the cursor, while live. */
static void
trees_cursor_Cursor_first_with_read_ahead_pad_0(void *frame_)
{
	trees_cursor_Cursor_first_with_read_ahead_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
}

/* trees_cursor_Cursor_first_with_read_ahead is first with the number of leaves its forward moves request ahead. */
bool
trees_cursor_Cursor_first_with_read_ahead(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, size_t leaves,
										  TreesCursorCursor *result, ChunksErrorError *error)
{
	TreesStoreTree tree_2 = trees_store_name(tree);
	trees_cursor_Cursor_first_with_read_ahead_frame_0_t frame = {.io = io, .pin = pin, .tree = tree_2, .leaves = leaves, .result = result, .error = error};

	std_landing(&frame, trees_cursor_Cursor_first_with_read_ahead_body_0, trees_cursor_Cursor_first_with_read_ahead_pad_0);
	return frame.ok;
}

/* trees_cursor_Cursor_set_read_ahead sets how many leaves past the one a forward move enters are requested ahead of being read, none for 0. */
void
trees_cursor_Cursor_set_read_ahead(TreesCursorCursor *self, size_t leaves)
{
	self->read_ahead = leaves;
}

/*
 * trees_cursor_Cursor_read_ahead requests the leaves at the offsets start
 * through end past the one the cursor is in, within its parent, an
 * inclusive range.
 */
static bool
trees_cursor_Cursor_read_ahead(const TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, size_t start,
							   size_t end, ChunksErrorError *error)
{
	size_t		d;
	TreesCursorFrame parent;
	TreesNodeNode node;

	if (!std_checked_sub_usize(self->path.len, 2, &d))
		return true;
	std_index_check(d, self->path.len, RUST_FILE, 240, 76);
	parent = STD_VEC_AT(&self->path, TreesCursorFrame, d);
	if (parent.level != 1)
		return true;
	if (!trees_pin_Pin_node(pin, io, TREES_STORE_TREE_NAMED(&parent.tree), &node, error))
		return false;
	for (size_t o = start, exhausted = start > end; !exhausted;)
	{
		size_t		o_2 = o;
		size_t		i;
		TreesStoreTree child;

		if (o < end)
			o = std_forward_unchecked_usize_Checked(o, 1);
		else
			exhausted = true;
		i = STD_ADD(usize, parent.idx, o_2, 245, 34);
		if (!(i < parent.len))
			break;
		child = trees_node_Node_child_tree(&node, i);
		if (!trees_pin_Pin_prefetch(pin, io, TREES_STORE_TREE_NAMED(&child), error))
			return false;
	}
	return true;
}

/* trees_cursor_Cursor_last_closure_0 is last's choice of child and of position: the node's last entry. */
static bool
trees_cursor_Cursor_last_closure_0(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
								   ChunksErrorError *error)
{
	*result = std_saturating_sub_usize(trees_node_Node_len(&node), 1);
	return true;
}

/* trees_cursor_Cursor_last_frame_0_t is last's state across its pad: its arguments, its result, and the cursor it builds, while live. */
typedef struct trees_cursor_Cursor_last_frame_0_t
{
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesStoreTree tree;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TreesCursorCursor *result;
	ChunksErrorError *error;
	bool		ok;
} trees_cursor_Cursor_last_frame_0_t;

/* trees_cursor_Cursor_last_body_0 is last's body. */
static void
trees_cursor_Cursor_last_body_0(void *frame_)
{
	trees_cursor_Cursor_last_frame_0_t *frame = frame_;

	if (!trees_cursor_Cursor_start(frame->io, frame->pin, frame->tree, &frame->cursor, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->cursor_live = true;
	if (!trees_cursor_Cursor_descend(&frame->cursor, frame->io, frame->pin, 0, trees_cursor_Cursor_last_closure_0, NULL,
									 trees_cursor_Cursor_last_closure_0, NULL, false, frame->error))
	{
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
		frame->ok = false;
		return;
	}
	frame->cursor_live = false;
	*frame->result = frame->cursor;
	frame->ok = true;
}

/* trees_cursor_Cursor_last_pad_0 is last's pad: the cursor, while live. */
static void
trees_cursor_Cursor_last_pad_0(void *frame_)
{
	trees_cursor_Cursor_last_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
}

/* trees_cursor_Cursor_last returns a leaf cursor at the tree's last entry. */
bool
trees_cursor_Cursor_last(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, TreesCursorCursor *result,
						 ChunksErrorError *error)
{
	TreesStoreTree tree_2 = trees_store_name(tree);
	trees_cursor_Cursor_last_frame_0_t frame = {.io = io, .pin = pin, .tree = tree_2, .result = result, .error = error};

	std_landing(&frame, trees_cursor_Cursor_last_body_0, trees_cursor_Cursor_last_pad_0);
	return frame.ok;
}

/* trees_cursor_Cursor_at_ordinal_frame_0_t is at_ordinal's state across its pad: the cursor it builds, while live. */
typedef struct trees_cursor_Cursor_at_ordinal_frame_0_t
{
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesStoreNamed tree;
	uint64		ordinal;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TreesNodeFlat flat;
	bool		flat_live;
	TreesCursorCursor *result;
	ChunksErrorError *error;
	bool		ok;
} trees_cursor_Cursor_at_ordinal_frame_0_t;

/* trees_cursor_Cursor_at_ordinal_body_0 is at_ordinal's body. */
static void
trees_cursor_Cursor_at_ordinal_body_0(void *frame_)
{
	trees_cursor_Cursor_at_ordinal_frame_0_t *frame = frame_;
	TreesStoreTree tree_2 = trees_store_name(frame->tree);
	uint64		rest;

	if (!trees_cursor_Cursor_start(frame->io, frame->pin, tree_2, &frame->cursor, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->cursor_live = true;
	rest = frame->ordinal;
	for (;;)
	{
		size_t		last = STD_SUB(usize, frame->cursor.path.len, 1, 271, 24);
		TreesCursorFrame cursor_frame;
		TreesNodeNode node;
		size_t		idx;
		bool		found = false;
		TreesStoreTree child;
		TreesNodeNode child_node;
		TreesCursorFrame pushed;

		std_index_check(last, frame->cursor.path.len, RUST_FILE, 272, 36);
		cursor_frame = STD_VEC_AT(&frame->cursor.path, TreesCursorFrame, last);
		if (!trees_pin_Pin_node(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&cursor_frame.tree), &node,
								frame->error))
		{
			frame->cursor_live = false;
			trees_cursor_Cursor_drop(&frame->cursor);
			frame->ok = false;
			return;
		}
		if (trees_node_Node_is_leaf(&node))
		{
			size_t		placed = (size_t) std_min_u64(rest, (uint64) trees_node_Node_len(&node));

			std_index_check(last, frame->cursor.path.len, RUST_FILE, 275, 28);
			STD_VEC_AT(&frame->cursor.path, TreesCursorFrame, last).idx = placed;
			std_index_check(last, frame->cursor.path.len, RUST_FILE, 276, 43);
			frame->cursor.valid = STD_VEC_AT(&frame->cursor.path, TreesCursorFrame, last).idx <
				trees_node_Node_len(&node);
			if (!trees_pin_Pin_own(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&cursor_frame.tree), &frame->flat,
								   frame->error))
			{
				frame->cursor_live = false;
				trees_cursor_Cursor_drop(&frame->cursor);
				frame->ok = false;
				return;
			}
			frame->flat_live = true;
			if (frame->cursor.has_node)
				trees_node_Flat_drop(&frame->cursor.node);
			frame->flat_live = false;
			frame->cursor.node = frame->flat;
			frame->cursor.has_node = true;
			std_OnceCell_drop_ArcSliceU8(&frame->cursor.copy);
			std_OnceCell_new_ArcSliceU8(&frame->cursor.copy);
			frame->cursor_live = false;
			*frame->result = frame->cursor;
			frame->ok = true;
			return;
		}
		for (size_t i = 0, i_end = trees_node_Node_len(&node); i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
		{
			if (trees_node_Node_before(&node, STD_ADD(usize, i, 1, 281, 61)) > rest)
			{
				idx = i;
				found = true;
				break;
			}
		}
		{
			size_t		default_ = std_saturating_sub_usize(trees_node_Node_len(&node), 1);

			if (!found)
				idx = default_;
		}
		rest = STD_SUB(u64, rest, std_min_u64(trees_node_Node_before(&node, idx), rest), 282, 13);
		std_index_check(last, frame->cursor.path.len, RUST_FILE, 283, 24);
		STD_VEC_AT(&frame->cursor.path, TreesCursorFrame, last).idx = idx;
		child = trees_node_Node_child_tree(&node, idx);
		if (!trees_pin_Pin_node(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&child), &child_node, frame->error))
		{
			frame->cursor_live = false;
			trees_cursor_Cursor_drop(&frame->cursor);
			frame->ok = false;
			return;
		}
		pushed = trees_cursor_Frame_of(child_node, child, 0);
		std_vec_push_Checked(&frame->cursor.path, &pushed, sizeof(TreesCursorFrame), _Alignof(TreesCursorFrame));
	}
}

/*
 * trees_cursor_Cursor_at_ordinal_pad_0 is at_ordinal's pad: the new read
 * stored should the old node's drop unwind, as an assignment stores it, then
 * the cursor, while live.
 */
static void
trees_cursor_Cursor_at_ordinal_pad_0(void *frame_)
{
	trees_cursor_Cursor_at_ordinal_frame_0_t *frame = frame_;

	if (frame->flat_live)
	{
		frame->cursor.node = frame->flat;
		frame->cursor.has_node = true;
	}
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
}

/* trees_cursor_Cursor_at_ordinal returns a leaf cursor at the entry with the position in key order, counting from zero. */
bool
trees_cursor_Cursor_at_ordinal(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, uint64 ordinal,
							   TreesCursorCursor *result, ChunksErrorError *error)
{
	trees_cursor_Cursor_at_ordinal_frame_0_t frame = {.io = io, .pin = pin, .tree = tree, .ordinal = ordinal,
	.result = result, .error = error};

	std_landing(&frame, trees_cursor_Cursor_at_ordinal_body_0, trees_cursor_Cursor_at_ordinal_pad_0);
	return frame.ok;
}

/* trees_cursor_Cursor_is_valid reports whether the cursor is at an entry. */
bool
trees_cursor_Cursor_is_valid(const TreesCursorCursor *self)
{
	return self->valid;
}

/* trees_cursor_Cursor_bottom returns the cursor's own frame. */
static const TreesCursorFrame *
trees_cursor_Cursor_bottom(const TreesCursorCursor *self)
{
	if (self->path.len == 0)
		STD_OPTION_UNWRAP_FAILED(296, 26);
	return &STD_VEC_AT(&self->path, TreesCursorFrame, std_wrapping_sub_usize(self->path.len, 1));
}

/* trees_cursor_Cursor_node returns the node the cursor is in. */
TreesNodeNode
trees_cursor_Cursor_node(const TreesCursorCursor *self)
{
	if (!self->has_node)
		STD_OPTION_EXPECT_FAILED(301, 28, "a cursor reaches its node when it is made");
	return trees_node_Flat_node(&self->node);
}

/* trees_cursor_Cursor_shared_closure_0 is shared's copy: the node's flat bytes in an Arc<[u8]>. */
static void
trees_cursor_Cursor_shared_closure_0(void *captures, StdArcSlice *result)
{
	const TreesNodeFlat *flat = captures;
	StdBytes	bytes = host_cache_Read_Deref_deref(&flat->f0);

	*result = std_arc_from_slice(bytes.ptr, bytes.len, 1, 1);
}

/*
 * trees_cursor_Cursor_shared returns the flat bytes of the node the cursor
 * is in for a holder that outlives the cursor's stay there, copying them
 * once for all such holders, since the cursor's own read ends when it moves
 * on; the Arc is cloned because each holder keeps its own count.
 */
StdArcSlice
trees_cursor_Cursor_shared(TreesCursorCursor *self)
{
	const StdArcSlice *copy;
	StdArcSlice result;

	if (!self->has_node)
		STD_OPTION_EXPECT_FAILED(307, 39, "a cursor reaches its node when it is made");
	copy = std_OnceCell_get_or_init_ArcSliceU8(&self->copy, trees_cursor_Cursor_shared_closure_0, &self->node,
											   NULL);
	result.ptr = std_arc_clone(copy->ptr);
	result.len = copy->len;
	return result;
}

/* trees_cursor_Cursor_node_tree returns the name of the node the cursor is in. */
TreesStoreTree
trees_cursor_Cursor_node_tree(const TreesCursorCursor *self)
{
	return trees_cursor_Cursor_bottom(self)->tree;
}

/* trees_cursor_Cursor_index returns the cursor's position in its node. */
size_t
trees_cursor_Cursor_index(const TreesCursorCursor *self)
{
	return trees_cursor_Cursor_bottom(self)->idx;
}

/*
 * trees_cursor_Cursor_find_in_node moves the cursor to the entry of its own
 * node whose key is the key, by a search of that node alone, and answers
 * whether the node holds one, leaving the cursor where it was otherwise: a
 * tree's keys are unique, so an entry found is the tree's entry of the key.
 */
bool
trees_cursor_Cursor_find_in_node(TreesCursorCursor *self, HostIoIo *io, const uint8 *key, size_t key_len,
								 const TreesKeyOrder *order, bool *result, ChunksErrorError *error)
{
	TreesNodeNode node = trees_cursor_Cursor_node(self);
	size_t		at;
	StdBytes	found;

	*result = false;
	if (!trees_node_Node_search(&node, io, key, key_len, order, &at, error))
		return false;
	if (at >= trees_node_Node_len(&node))
		return true;
	found = trees_node_Node_key(&node, at);
	if (found.len != key_len || memcmp(found.ptr, key, key_len) != 0)
		return true;
	STD_VEC_AT(&self->path, TreesCursorFrame, std_wrapping_sub_usize(self->path.len, 1)).idx = at;
	self->valid = true;
	*result = true;
	return true;
}

/* trees_cursor_Cursor_key returns the key of the entry the cursor is at. */
StdBytes
trees_cursor_Cursor_key(const TreesCursorCursor *self)
{
	TreesNodeNode node = trees_cursor_Cursor_node(self);

	return trees_node_Node_key(&node, trees_cursor_Cursor_index(self));
}

/* trees_cursor_Cursor_value returns the value of the leaf entry the cursor is at. */
StdBytes
trees_cursor_Cursor_value(const TreesCursorCursor *self)
{
	TreesNodeNode node = trees_cursor_Cursor_node(self);

	return trees_node_Node_value(&node, trees_cursor_Cursor_index(self));
}

/* trees_cursor_Cursor_child_tree returns the name of the child of the internal entry the cursor is at. */
TreesStoreTree
trees_cursor_Cursor_child_tree(const TreesCursorCursor *self)
{
	TreesNodeNode node = trees_cursor_Cursor_node(self);
	size_t		i = trees_cursor_Cursor_index(self);

	return trees_node_Node_child_tree(&node, i);
}

/*
 * trees_cursor_Cursor_stamps returns the row stamps of the leaf entry the
 * cursor is at, one for each copy of a row it counts, which the cursor's
 * node holds decoded.
 */
bool
trees_cursor_Cursor_stamps(const TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, StdVec *result,
						   ChunksErrorError *error)
{
	TreesNodeNode node = trees_cursor_Cursor_node(self);
	ChunksTrailerStamps stamps = trees_node_Node_stamps_view(&node);

	chunks_trailer_Stamps_entry(&stamps, trees_cursor_Cursor_index(self), result);
	return true;
}

/*
 * trees_cursor_Cursor_values returns the value ids of the row of the leaf
 * entry the cursor is at, in the row's order, none for a row with no value
 * out of line, which the cursor's node holds decoded.
 */
bool
trees_cursor_Cursor_values(const TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, StdVec *result,
						   ChunksErrorError *error)
{
	TreesNodeNode node = trees_cursor_Cursor_node(self);
	size_t		values_len;
	const uint32 *values = trees_node_Node_values(&node, trees_cursor_Cursor_index(self), &values_len);

	std_slice_to_vec_Checked(result, values, values_len, sizeof(uint32), _Alignof(uint32));
	return true;
}

/* trees_cursor_Cursor_at_node_end reports whether the cursor is at its node's last entry. */
bool
trees_cursor_Cursor_at_node_end(const TreesCursorCursor *self)
{
	size_t		next = STD_ADD(usize, trees_cursor_Cursor_index(self), 1, 351, 9);
	size_t		len = trees_cursor_Cursor_bottom(self)->len;

	return next == len;
}

/* trees_cursor_Cursor_depth_of returns the position on the path of the node at the level, if the path reaches that level. */
static bool
trees_cursor_Cursor_depth_of(const TreesCursorCursor *self, uint8 level, size_t *result)
{
	uint8		top;
	uint8		bottom;

	std_index_check(0, self->path.len, RUST_FILE, 356, 39);
	top = STD_VEC_AT(&self->path, TreesCursorFrame, 0).level;
	bottom = trees_cursor_Cursor_bottom(self)->level;
	if (!(level >= bottom && level <= top))
		return false;
	*result = (size_t) STD_SUB(u8, top, level, 357, 51);
	return true;
}

/* trees_cursor_Cursor_tree_at returns the name of the node on the cursor's path at the level, false when the path lacks the level. */
bool
trees_cursor_Cursor_tree_at(const TreesCursorCursor *self, uint8 level, TreesStoreTree *result)
{
	size_t		d;

	if (!trees_cursor_Cursor_depth_of(self, level, &d))
		return false;
	std_index_check(d, self->path.len, RUST_FILE, 362, 47);
	*result = STD_VEC_AT(&self->path, TreesCursorFrame, d).tree;
	return true;
}

/* trees_cursor_Cursor_starts_node reports whether the cursor is at the first entry under the node on its path at the level. */
bool
trees_cursor_Cursor_starts_node(const TreesCursorCursor *self, uint8 level)
{
	size_t		d;

	if (!trees_cursor_Cursor_depth_of(self, level, &d))
		return false;
	std_range_from_check(d, self->path.len, RUST_FILE, 367, 55);
	for (size_t k = d, k_len = self->path.len; k < k_len; k++)
		if (STD_VEC_AT(&self->path, TreesCursorFrame, k).idx != 0)
			return false;
	return true;
}

/* trees_cursor_Cursor_ordinal returns the position of the leaf entry the cursor is at, in key order. */
bool
trees_cursor_Cursor_ordinal(const TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, uint64 *result,
							ChunksErrorError *error)
{
	uint64		ordinal = 0;

	for (size_t k = 0, k_len = self->path.len; k < k_len; k++)
	{
		const TreesCursorFrame *frame = &STD_VEC_AT(&self->path, TreesCursorFrame, k);
		uint64		add;

		if (frame->level == 0)
			add = (uint64) frame->idx;
		else
		{
			TreesStoreTree tree = frame->tree;
			TreesNodeNode node;

			if (!trees_pin_Pin_node(pin, io, TREES_STORE_TREE_NAMED(&tree), &node, error))
				return false;
			add = trees_node_Node_before(&node, frame->idx);
		}
		ordinal = STD_ADD(u64, ordinal, add, 374, 13);
	}
	*result = ordinal;
	return true;
}

/*
 * trees_cursor_Cursor_next_in_node moves to the next entry within the
 * cursor's node and reports whether it did, false at the node's end or on
 * an invalid cursor.
 */
bool
trees_cursor_Cursor_next_in_node(TreesCursorCursor *self)
{
	bool		valid = self->valid;
	TreesCursorFrame *frame;

	if (self->path.len == 0)
		STD_OPTION_EXPECT_FAILED(386, 42, "a cursor's path holds its node");
	frame = &STD_VEC_AT(&self->path, TreesCursorFrame, std_wrapping_sub_usize(self->path.len, 1));
	if (valid && STD_ADD(usize, frame->idx, 1, 387, 21) < frame->len)
	{
		frame->idx = STD_ADD(usize, frame->idx, 1, 388, 13);
		return true;
	}
	return false;
}

/*
 * trees_cursor_Cursor_prev_in_node moves to the previous entry within the
 * cursor's node and reports whether it did, false at the node's start or on
 * an invalid cursor.
 */
bool
trees_cursor_Cursor_prev_in_node(TreesCursorCursor *self)
{
	bool		valid = self->valid;
	TreesCursorFrame *frame;

	if (self->path.len == 0)
		STD_OPTION_EXPECT_FAILED(398, 42, "a cursor's path holds its node");
	frame = &STD_VEC_AT(&self->path, TreesCursorFrame, std_wrapping_sub_usize(self->path.len, 1));
	if (valid && frame->idx > 0)
	{
		frame->idx = STD_SUB(usize, frame->idx, 1, 400, 13);
		return true;
	}
	return false;
}

static bool trees_cursor_Cursor_advance_at(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, size_t depth,
										   ChunksErrorError *error);

/* trees_cursor_Cursor_next moves to the next entry at the cursor's level. */
bool
trees_cursor_Cursor_next(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	size_t		last;

	if (!self->valid || trees_cursor_Cursor_next_in_node(self))
		return true;
	last = STD_SUB(usize, self->path.len, 1, 411, 20);
	if (!trees_cursor_Cursor_advance_at(self, io, pin, last, error))
		return false;
	if (self->valid && self->read_ahead > 0)
		return trees_cursor_Cursor_read_ahead(self, io, pin, self->read_ahead, self->read_ahead, error);
	return true;
}

/* TreesCursorCursorAdvanceToClosure0 is advance_to's choice of child's captures: the key and the order, by reference. */
typedef struct TreesCursorCursorAdvanceToClosure0
{
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
} TreesCursorCursorAdvanceToClosure0;

/* TreesCursorCursorAdvanceToClosure1 is advance_to's position in the level's node's captures: the key and the order, by reference. */
typedef struct TreesCursorCursorAdvanceToClosure1
{
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
} TreesCursorCursorAdvanceToClosure1;

/* trees_cursor_Cursor_advance_to_closure_0 is advance_to's choice of child: the key's position, at most the node's last. */
static bool
trees_cursor_Cursor_advance_to_closure_0(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
										 ChunksErrorError *error)
{
	TreesCursorCursorAdvanceToClosure0 *closure = captures;
	size_t		found;

	if (!trees_node_Node_search(&node, io, closure->key, closure->key_len, closure->order, &found, error))
		return false;
	*result = std_min_usize(found, std_saturating_sub_usize(trees_node_Node_len(&node), 1));
	return true;
}

/* trees_cursor_Cursor_advance_to_closure_1 is advance_to's position in the level's node: the key's. */
static bool
trees_cursor_Cursor_advance_to_closure_1(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
										 ChunksErrorError *error)
{
	TreesCursorCursorAdvanceToClosure1 *closure = captures;

	return trees_node_Node_search(&node, io, closure->key, closure->key_len, closure->order, result, error);
}

/*
 * trees_cursor_Cursor_advance_to moves the cursor forward to the first
 * entry at its level whose key is not below the key, climbing only as far
 * as the nodes on its path end before the key and descending from there as
 * seek does; a key at or before the cursor's leaves it where it is.
 */
bool
trees_cursor_Cursor_advance_to(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, const uint8 *key,
							   size_t key_len, const TreesKeyOrder *order, ChunksErrorError *error)
{
	StdOrdering ordering;
	size_t		bottom;
	TreesCursorFrame frame;
	uint8		level;
	size_t		depth;
	TreesCursorCursorAdvanceToClosure0 choose = {.key = key, .key_len = key_len, .order = order};
	TreesCursorCursorAdvanceToClosure1 place = {.key = key, .key_len = key_len, .order = order};

	if (!self->valid)
		return true;
	{
		StdBytes	at = trees_cursor_Cursor_key(self);

		if (!trees_KeyOrder_compare(order, io, at.ptr, at.len, key, key_len, &ordering, error))
			return false;
		if (ordering != STD_ORDERING_LESS)
			return true;
	}
	bottom = STD_SUB(usize, self->path.len, 1, 426, 22);
	std_index_check(bottom, self->path.len, RUST_FILE, 427, 30);
	frame = STD_VEC_AT(&self->path, TreesCursorFrame, bottom);
	if (self->has_node)
	{
		TreesNodeNode node = trees_node_Flat_node(&self->node);
		StdBytes	end = trees_node_Node_key(&node, STD_SUB(usize, frame.len, 1, 430, 43));

		if (!trees_KeyOrder_compare(order, io, end.ptr, end.len, key, key_len, &ordering, error))
			return false;
		if (ordering != STD_ORDERING_LESS)
		{
			size_t		found;

			if (!trees_node_Node_search_from(&node, io, key, key_len, order,
											 STD_ADD(usize, frame.idx, 1, 431, 74), &found, error))
				return false;
			std_index_check(bottom, self->path.len, RUST_FILE, 431, 26);
			STD_VEC_AT(&self->path, TreesCursorFrame, bottom).idx = found;
			return true;
		}
	}
	level = trees_cursor_Cursor_bottom(self)->level;
	depth = bottom;
	for (;;)
	{
		TreesCursorFrame frame_2;
		TreesNodeNode node;
		StdBytes	end;

		std_index_check(depth, self->path.len, RUST_FILE, 438, 34);
		frame_2 = STD_VEC_AT(&self->path, TreesCursorFrame, depth);
		if (!trees_pin_Pin_node(pin, io, TREES_STORE_TREE_NAMED(&frame_2.tree), &node, error))
			return false;
		end = trees_node_Node_key(&node, STD_SUB(usize, frame_2.len, 1, 440, 43));
		if (!trees_KeyOrder_compare(order, io, end.ptr, end.len, key, key_len, &ordering, error))
			return false;
		if (ordering != STD_ORDERING_LESS)
		{
			size_t		found;

			if (!trees_node_Node_search(&node, io, key, key_len, order, &found, error))
				return false;
			std_index_check(depth, self->path.len, RUST_FILE, 441, 26);
			STD_VEC_AT(&self->path, TreesCursorFrame, depth).idx = found;
			break;
		}
		if (depth == 0)
		{
			self->valid = false;
			return true;
		}
		depth = STD_SUB(usize, depth, 1, 448, 13);
	}
	std_vec_truncate(&self->path, STD_ADD(usize, depth, 1, 450, 28), sizeof(TreesCursorFrame), NULL);
	return trees_cursor_Cursor_descend(self, io, pin, level, trees_cursor_Cursor_advance_to_closure_0, &choose,
									   trees_cursor_Cursor_advance_to_closure_1, &place, true, error);
}

/* TreesCursorCursorAdvanceToNodeClosure0 is advance_to_node's choice of child's captures: the key and the order, by reference. */
typedef struct TreesCursorCursorAdvanceToNodeClosure0
{
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
} TreesCursorCursorAdvanceToNodeClosure0;

/* trees_cursor_Cursor_advance_to_node_closure_0 is advance_to_node's choice of child: the key's position, at most the node's last. */
static bool
trees_cursor_Cursor_advance_to_node_closure_0(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
											  ChunksErrorError *error)
{
	TreesCursorCursorAdvanceToNodeClosure0 *closure = captures;
	size_t		found;

	if (!trees_node_Node_search(&node, io, closure->key, closure->key_len, closure->order, &found, error))
		return false;
	*result = std_min_usize(found, std_saturating_sub_usize(trees_node_Node_len(&node), 1));
	return true;
}

/* trees_cursor_Cursor_advance_to_node_closure_1 is advance_to_node's position in the level's node: its first entry. */
static bool
trees_cursor_Cursor_advance_to_node_closure_1(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
											  ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/*
 * trees_cursor_Cursor_advance_to_node moves the cursor forward to the first
 * entry of the node at its level that holds the key, or would hold it, as
 * seek_node finds it, climbing only as far as the nodes on its path end
 * before the key; a key its own node holds leaves the cursor where it is.
 */
bool
trees_cursor_Cursor_advance_to_node(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, const uint8 *key,
									size_t key_len, const TreesKeyOrder *order, ChunksErrorError *error)
{
	size_t		bottom = STD_SUB(usize, self->path.len, 1, 465, 22);
	size_t		depth = bottom;
	uint8		level;
	TreesCursorCursorAdvanceToNodeClosure0 choose = {.key = key, .key_len = key_len, .order = order};

	for (;;)
	{
		TreesCursorFrame frame;
		TreesNodeNode node;
		bool		holds = depth == 0;

		std_index_check(depth, self->path.len, RUST_FILE, 468, 34);
		frame = STD_VEC_AT(&self->path, TreesCursorFrame, depth);
		if (!trees_pin_Pin_node(pin, io, TREES_STORE_TREE_NAMED(&frame.tree), &node, error))
			return false;
		if (!holds)
		{
			StdBytes	end = trees_node_Node_key(&node, STD_SUB(usize, frame.len, 1, 470, 57));
			StdOrdering ordering;

			if (!trees_KeyOrder_compare(order, io, end.ptr, end.len, key, key_len, &ordering, error))
				return false;
			holds = ordering != STD_ORDERING_LESS;
		}
		if (holds)
		{
			size_t		found;

			if (depth == bottom)
				return true;
			if (!trees_node_Node_search(&node, io, key, key_len, order, &found, error))
				return false;
			found = std_min_usize(found, STD_SUB(usize, frame.len, 1, 474, 73));
			std_index_check(depth, self->path.len, RUST_FILE, 474, 26);
			STD_VEC_AT(&self->path, TreesCursorFrame, depth).idx = found;
			break;
		}
		depth = STD_SUB(usize, depth, 1, 477, 13);
	}
	level = trees_cursor_Cursor_bottom(self)->level;
	std_vec_truncate(&self->path, STD_ADD(usize, depth, 1, 480, 28), sizeof(TreesCursorFrame), NULL);
	return trees_cursor_Cursor_descend(self, io, pin, level, trees_cursor_Cursor_advance_to_node_closure_0, &choose,
									   trees_cursor_Cursor_advance_to_node_closure_1, NULL, true, error);
}

/* trees_cursor_Cursor_skip_node moves to the first entry after the node on the cursor's path at the level. */
bool
trees_cursor_Cursor_skip_node(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, uint8 level,
							  ChunksErrorError *error)
{
	size_t		depth;

	if (!trees_cursor_Cursor_depth_of(self, level, &depth))
		return true;
	if (depth == 0)
		self->valid = false;
	else if (self->valid)
		return trees_cursor_Cursor_advance_at(self, io, pin, STD_SUB(usize, depth, 1, 489, 67), error);
	return true;
}

/* trees_cursor_Cursor_advance_at_closure_0 is advance_at's choice of child: the first. */
static bool
trees_cursor_Cursor_advance_at_closure_0(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
										 ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/* trees_cursor_Cursor_advance_at_closure_1 is advance_at's position in the level's node: the first. */
static bool
trees_cursor_Cursor_advance_at_closure_1(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
										 ChunksErrorError *error)
{
	*result = 0;
	return true;
}

/*
 * trees_cursor_Cursor_advance_at moves the frame at the depth to its next
 * entry, climbing past the ends of nodes, and then back down to the first
 * entry under it at the cursor's level.
 */
static bool
trees_cursor_Cursor_advance_at(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, size_t depth,
							   ChunksErrorError *error)
{
	uint8		level = trees_cursor_Cursor_bottom(self)->level;
	size_t		depth_2 = depth;

	for (;;)
	{
		TreesCursorFrame *frame;

		std_index_check(depth_2, self->path.len, RUST_FILE, 501, 39);
		frame = &STD_VEC_AT(&self->path, TreesCursorFrame, depth_2);
		frame->idx = STD_ADD(usize, frame->idx, 1, 502, 13);
		if (frame->idx < frame->len)
			break;
		if (depth_2 == 0)
		{
			self->valid = false;
			return true;
		}
		depth_2 = STD_SUB(usize, depth_2, 1, 510, 13);
	}
	std_vec_truncate(&self->path, STD_ADD(usize, depth_2, 1, 512, 28), sizeof(TreesCursorFrame), NULL);
	return trees_cursor_Cursor_descend(self, io, pin, level, trees_cursor_Cursor_advance_at_closure_0, NULL,
									   trees_cursor_Cursor_advance_at_closure_1, NULL, true, error);
}

/* trees_cursor_Cursor_prev_closure_0 is prev's choice of child and of position: the node's last entry. */
static bool
trees_cursor_Cursor_prev_closure_0(void *captures, HostIoIo *io, TreesNodeNode node, size_t *result,
								   ChunksErrorError *error)
{
	*result = std_saturating_sub_usize(trees_node_Node_len(&node), 1);
	return true;
}

/* trees_cursor_Cursor_prev moves to the previous entry at the cursor's level. */
bool
trees_cursor_Cursor_prev(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	uint8		level;
	size_t		depth;

	if (!self->valid || trees_cursor_Cursor_prev_in_node(self))
		return true;
	level = trees_cursor_Cursor_bottom(self)->level;
	depth = STD_SUB(usize, self->path.len, 1, 522, 25);
	for (;;)
	{
		TreesCursorFrame *frame;

		std_index_check(depth, self->path.len, RUST_FILE, 524, 39);
		frame = &STD_VEC_AT(&self->path, TreesCursorFrame, depth);
		if (frame->idx > 0)
		{
			frame->idx = STD_SUB(usize, frame->idx, 1, 526, 17);
			break;
		}
		if (depth == 0)
		{
			self->valid = false;
			return true;
		}
		depth = STD_SUB(usize, depth, 1, 533, 13);
	}
	if (depth == STD_SUB(usize, self->path.len, 1, 535, 21))
		return true;
	std_vec_truncate(&self->path, STD_ADD(usize, depth, 1, 538, 28), sizeof(TreesCursorFrame), NULL);
	return trees_cursor_Cursor_descend(self, io, pin, level, trees_cursor_Cursor_prev_closure_0, NULL,
									   trees_cursor_Cursor_prev_closure_0, NULL, true, error);
}

/* trees_cursor_Cursor_locator returns where the node the cursor is in was stored. */
ChunksTrailerLocator
trees_cursor_Cursor_locator(const TreesCursorCursor *self)
{
	return trees_cursor_Cursor_bottom(self)->tree.locator;
}

/* trees_cursor_LeafStamps_Default_default is LeafStamps' derived Default: no leaf's stamps. */
TreesCursorLeafStamps
trees_cursor_LeafStamps_Default_default(void)
{
	TreesCursorLeafStamps result;

	result.has_f0 = false;
	return result;
}

/* trees_cursor_LeafStamps_of_frame_0_t is of's and stamp_of's state across their pad: the pair taken from the cache, while live, and their answers. */
typedef struct trees_cursor_LeafStamps_of_frame_0_t
{
	TreesCursorLeafStamps *self;
	HostIoIo   *io;
	TreesPinPin *pin;
	const TreesCursorCursor *cursor;
	trees_TupleTreeArcStamps_t taken;
	bool		taken_live;
	StdVec	   *result;
	uint64		tuple;
	uint64	   *copies;
	bool	   *some;
	ChunksTrailerStamp *stamp;
	ChunksErrorError *error;
	bool		ok;
} trees_cursor_LeafStamps_of_frame_0_t;

/*
 * trees_cursor_LeafStamps_read is of's and stamp_of's read: the cached pair
 * taken, put back when it is the cursor's leaf's, or else the leaf's stamps
 * read and stored, the taken pair dropped after, at the end of the let.
 */
static bool
trees_cursor_LeafStamps_read(trees_cursor_LeafStamps_of_frame_0_t *frame)
{
	TreesStoreTree node = trees_cursor_Cursor_node_tree(frame->cursor);

	frame->taken_live = frame->self->has_f0;
	frame->taken = frame->self->f0;
	frame->self->has_f0 = false;
	if (frame->taken_live && trees_store_Tree_PartialEq_eq(&frame->taken.f0, &node))
	{
		frame->taken_live = false;
		frame->self->f0 = frame->taken;
		frame->self->has_f0 = true;
	}
	else
	{
		StdArcInner *read;

		if (!trees_pin_Pin_stamps(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&node), &read, frame->error))
		{
			if (frame->taken_live)
			{
				frame->taken_live = false;
				trees_cursor_ArcStamps_drop(frame->taken.f1);
			}
			return false;
		}
		frame->self->f0.f0 = node;
		frame->self->f0.f1 = read;
		frame->self->has_f0 = true;
		if (frame->taken_live)
		{
			frame->taken_live = false;
			trees_cursor_ArcStamps_drop(frame->taken.f1);
		}
	}
	return true;
}

/* trees_cursor_LeafStamps_of_body_0 is of's body. */
static void
trees_cursor_LeafStamps_of_body_0(void *frame_)
{
	trees_cursor_LeafStamps_of_frame_0_t *frame = frame_;

	if (!trees_cursor_LeafStamps_read(frame))
		return;
	chunks_trailer_Stamps_entry(std_arc_Deref_deref(frame->self->f0.f1, _Alignof(ChunksTrailerStamps)),
								trees_cursor_Cursor_index(frame->cursor), frame->result);
	frame->ok = true;
}

/* trees_cursor_LeafStamps_stamp_of_body_0 is stamp_of's body. */
static void
trees_cursor_LeafStamps_stamp_of_body_0(void *frame_)
{
	trees_cursor_LeafStamps_of_frame_0_t *frame = frame_;
	const ChunksTrailerStamps *stamps;
	size_t		index;
	const uint32 *values;
	size_t		values_len;

	if (!trees_cursor_LeafStamps_read(frame))
		return;
	stamps = std_arc_Deref_deref(frame->self->f0.f1, _Alignof(ChunksTrailerStamps));
	index = trees_cursor_Cursor_index(frame->cursor);
	*frame->copies = chunks_trailer_Stamps_entry_len(stamps, index);
	*frame->some = chunks_trailer_Stamps_stamp_of(stamps, index, frame->tuple, frame->stamp);
	values = chunks_trailer_Stamps_values(stamps, index, &values_len);
	std_slice_to_vec_Checked(frame->result, values, values_len, sizeof(uint32), _Alignof(uint32));
	frame->ok = true;
}

/* trees_cursor_LeafStamps_of_pad_0 is of's pad: the taken pair, while live. */
static void
trees_cursor_LeafStamps_of_pad_0(void *frame_)
{
	trees_cursor_LeafStamps_of_frame_0_t *frame = frame_;

	if (frame->taken_live)
		trees_cursor_ArcStamps_drop(frame->taken.f1);
}

/*
 * trees_cursor_LeafStamps_of returns the row stamps of the leaf entry a
 * cursor is at, as Cursor's stamps does, reading the leaf's stamps only
 * when the walk has moved to another leaf.
 */
bool
trees_cursor_LeafStamps_of(TreesCursorLeafStamps *self, HostIoIo *io, TreesPinPin *pin,
						   const TreesCursorCursor *cursor, StdVec *result, ChunksErrorError *error)
{
	trees_cursor_LeafStamps_of_frame_0_t frame = {.self = self, .io = io, .pin = pin, .cursor = cursor,
	.result = result, .error = error};

	std_landing(&frame, trees_cursor_LeafStamps_of_body_0, trees_cursor_LeafStamps_of_pad_0);
	return frame.ok;
}

/*
 * trees_cursor_LeafStamps_stamp_of returns the number of row stamps of the
 * leaf entry a cursor is at, one for each copy of a row it counts, its stamp
 * of a tuple id, or nothing when it has none, by a search of the leaf's
 * stamps, and its row's value ids as Cursor's values gives them, the leaf's
 * stamps read only when the cursor is in another leaf than the last.
 */
bool
trees_cursor_LeafStamps_stamp_of(TreesCursorLeafStamps *self, HostIoIo *io, TreesPinPin *pin,
								 const TreesCursorCursor *cursor, uint64 tuple, uint64 *copies, bool *some,
								 ChunksTrailerStamp *result, StdVec *values, ChunksErrorError *error)
{
	trees_cursor_LeafStamps_of_frame_0_t frame = {.self = self, .io = io, .pin = pin, .cursor = cursor,
	.result = values, .tuple = tuple, .copies = copies, .some = some, .stamp = result, .error = error};

	std_landing(&frame, trees_cursor_LeafStamps_stamp_of_body_0, trees_cursor_LeafStamps_of_pad_0);
	return frame.ok;
}

/* trees_cursor_LeafStamps_drop is LeafStamps' drop glue: its leaf's stamps, when it holds them. */
void
trees_cursor_LeafStamps_drop(TreesCursorLeafStamps *self)
{
	if (self->has_f0)
		trees_cursor_ArcStamps_drop(self->f0.f1);
}
