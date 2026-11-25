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
 * Pin scopes (trees' pin.rs): the scope in which tree code reads nodes from
 * a store, each node read once and kept until the scope ends, and the walk
 * that reads a batch of entries a scope at a time.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/foldhash.h"
#include "doltgres/std/num.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/pin.h"

/* TREES_PIN_WALK_BATCH is WALK_BATCH, how many entries walk reads in one scope. */
#define TREES_PIN_WALK_BATCH 4096

/* trees_TupleTreeBoxFlat_t is the tuple (Tree, Box<Flat>): an entry of a pin's nodes. */
typedef struct trees_TupleTreeBoxFlat_t
{
	TreesStoreTree f0;
	TreesNodeFlat *f1;
} trees_TupleTreeBoxFlat_t;

/* trees_pin_BoxFlat_drop is Box<Flat>'s drop glue: the read's, then the box freed. */
static void
trees_pin_BoxFlat_drop(void *value)
{
	TreesNodeFlat *boxed = *(TreesNodeFlat **) value;

	trees_node_Flat_drop(boxed);
	std_global_dealloc(boxed, sizeof(TreesNodeFlat), _Alignof(TreesNodeFlat));
}

/* TREES_PIN_NODES_TYPE is the table of a pin's nodes, a FrozenMap<Tree, Box<Flat>, AddressState>. */
const StdHashMapType TREES_PIN_NODES_TYPE = {
	STD_HASHMAP_SHAPE(trees_TupleTreeBoxFlat_t),
	.key_hash = trees_store_Tree_Hash_hash,
	.key_eq = trees_store_Tree_PartialEq_eq,
	.value_drop = trees_pin_BoxFlat_drop,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/* pin's instance at walk's R, Result<Option<bool>>. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionBool
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *has_result, bool *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS bool *has_result; bool *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .has_result = has_result, .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->has_result, frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* TreesPinWalkClosure0 is walk's batch's captures: the cursor and the function, both by reference. */
typedef struct TreesPinWalkClosure0
{
	TreesCursorCursor *cursor;
	bool		(*f) (void *captures, HostIoIo *io, TreesPinPin *pin, const TreesCursorCursor *cursor, bool *result,
					  ChunksErrorError *error);
	void	   *f_captures;
} TreesPinWalkClosure0;

/*
 * trees_pin_walk_closure_0 is walk's batch: up to a batch of entries from
 * the cursor's on, each given to the function and passed, stopping at the
 * tree's end or when the function returns true.
 */
static bool
trees_pin_walk_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, bool *has_result, bool *result,
						 ChunksErrorError *error)
{
	TreesPinWalkClosure0 *closure = captures;

	for (size_t n = 0, n_end = TREES_PIN_WALK_BATCH; n < n_end; n = std_forward_unchecked_usize_Checked(n, 1))
	{
		bool		done;

		if (!trees_cursor_Cursor_is_valid(closure->cursor))
		{
			*has_result = true;
			*result = false;
			return true;
		}
		if (!closure->f(closure->f_captures, io, pin, closure->cursor, &done, error))
			return false;
		if (done)
		{
			*has_result = true;
			*result = true;
			return true;
		}
		if (!trees_cursor_Cursor_next(closure->cursor, io, pin, error))
			return false;
	}
	*has_result = false;
	return true;
}

/* trees_pin_walk_frame_0_t is walk's state across its pad: its arguments and result, and the function while it holds it. */
typedef struct trees_pin_walk_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesCursorCursor *cursor;
	bool		(*f) (void *captures, HostIoIo *io, TreesPinPin *pin, const TreesCursorCursor *cursor, bool *result,
					  ChunksErrorError *error);
	void	   *f_captures;
	void		(*f_drop) (void *);
	bool		f_live;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
} trees_pin_walk_frame_0_t;

/* trees_pin_walk_body_0 is walk's body. */
static void
trees_pin_walk_body_0(void *frame_)
{
	trees_pin_walk_frame_0_t *frame = frame_;

	for (;;)
	{
		TreesPinWalkClosure0 captures = {.cursor = frame->cursor, .f = frame->f, .f_captures = frame->f_captures};
		bool		has_stopped;
		bool		stopped;

		if (!trees_pin_pin_ResultOptionBool(frame->io, frame->store, trees_pin_walk_closure_0, &captures, NULL,
											&has_stopped, &stopped, frame->error))
		{
			frame->f_live = false;
			if (frame->f_drop != NULL)
				frame->f_drop(frame->f_captures);
			frame->ok = false;
			return;
		}
		if (has_stopped)
		{
			*frame->result = stopped;
			frame->f_live = false;
			if (frame->f_drop != NULL)
				frame->f_drop(frame->f_captures);
			frame->ok = true;
			return;
		}
	}
}

/* trees_pin_walk_pad_0 is walk's pad: the function's captures, while it holds them. */
static void
trees_pin_walk_pad_0(void *frame_)
{
	trees_pin_walk_frame_0_t *frame = frame_;

	if (frame->f_live && frame->f_drop != NULL)
		frame->f_drop(frame->f_captures);
}

/*
 * trees_pin_walk runs a function, which it takes, on each entry from a
 * cursor's on, moving the cursor past it, until the function returns true,
 * and returns whether it did, reading a batch of entries a scope at a time.
 */
bool
trees_pin_walk(HostIoIo *io, TreesStoreNodeStore store, TreesCursorCursor *cursor,
			   bool (*f) (void *captures, HostIoIo *io, TreesPinPin *pin, const TreesCursorCursor *cursor,
						  bool *result, ChunksErrorError *error),
			   void *f_captures, void (*f_drop) (void *), bool *result, ChunksErrorError *error)
{
	trees_pin_walk_frame_0_t frame = {.io = io, .store = store, .cursor = cursor, .f = f, .f_captures = f_captures,
	.f_drop = f_drop, .f_live = true, .result = result, .error = error};

	std_landing(&frame, trees_pin_walk_body_0, trees_pin_walk_pad_0);
	return frame.ok;
}

/* trees_pin_Pin_node_frame_0_t is node's state across its pad: the read it holds before the scope keeps it. */
typedef struct trees_pin_Pin_node_frame_0_t
{
	TreesPinPin *self;
	HostIoIo   *io;
	TreesStoreNamed tree;
	TreesNodeFlat read_2;
	bool		read_2_live;
	TreesNodeNode *result;
	ChunksErrorError *error;
	bool		ok;
} trees_pin_Pin_node_frame_0_t;

/* trees_pin_Pin_node_body_0 is node's body. */
static void
trees_pin_Pin_node_body_0(void *frame_)
{
	trees_pin_Pin_node_frame_0_t *frame = frame_;
	TreesStoreTree tree_2 = trees_store_name(frame->tree);
	const TreesNodeFlat *read = std_FrozenMap_get(&frame->self->nodes, &TREES_PIN_NODES_TYPE, &STD_BOX_DEREF_OPS,
												  &tree_2);
	TreesNodeFlat *boxed;

	if (read != NULL)
	{
		*frame->result = trees_node_Flat_node(read);
		frame->ok = true;
		return;
	}
	if (!frame->self->store.ops->read(frame->self->store.self, frame->io, &tree_2.address, tree_2.locator,
									  &frame->read_2, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->read_2_live = true;
	boxed = std_box_new_uninit_layout_Checked(sizeof(TreesNodeFlat), _Alignof(TreesNodeFlat));
	*boxed = frame->read_2;
	frame->read_2_live = false;
	read = std_FrozenMap_insert(&frame->self->nodes, &TREES_PIN_NODES_TYPE, &STD_BOX_DEREF_OPS, &tree_2, &boxed);
	*frame->result = trees_node_Flat_node(read);
	frame->ok = true;
}

/* trees_pin_Pin_node_pad_0 is node's pad: the read, while it holds it. */
static void
trees_pin_Pin_node_pad_0(void *frame_)
{
	trees_pin_Pin_node_frame_0_t *frame = frame_;

	if (frame->read_2_live)
		trees_node_Flat_drop(&frame->read_2);
}

/* trees_pin_Pin_node returns the node a tree name names, read once in the scope. */
bool
trees_pin_Pin_node(TreesPinPin *self, HostIoIo *io, TreesStoreNamed tree, TreesNodeNode *result,
				   ChunksErrorError *error)
{
	trees_pin_Pin_node_frame_0_t frame = {.self = self, .io = io, .tree = tree, .result = result, .error = error};

	std_landing(&frame, trees_pin_Pin_node_body_0, trees_pin_Pin_node_pad_0);
	return frame.ok;
}

/*
 * trees_pin_Pin_own returns the node a tree name names as a read of its
 * holder's own, for one that outlives the scope, as a cursor's own node
 * does.
 */
bool
trees_pin_Pin_own(TreesPinPin *self, HostIoIo *io, TreesStoreNamed tree, TreesNodeFlat *result,
				  ChunksErrorError *error)
{
	TreesStoreTree tree_2 = trees_store_name(tree);

	return self->store.ops->read(self->store.self, io, &tree_2.address, tree_2.locator, result, error);
}

/* trees_pin_Pin_prefetch tells the store that the node a tree name names will be read soon, unless the scope read it. */
bool
trees_pin_Pin_prefetch(TreesPinPin *self, HostIoIo *io, TreesStoreNamed tree, ChunksErrorError *error)
{
	TreesStoreTree tree_2 = trees_store_name(tree);

	if (std_FrozenMap_get(&self->nodes, &TREES_PIN_NODES_TYPE, &STD_BOX_DEREF_OPS, &tree_2) != NULL)
		return true;
	return self->store.ops->prefetch(self->store.self, io, tree_2, error);
}

/* trees_pin_Pin_stamps_frame_0_t is stamps' state across its pad: the stamps it holds before their Arc does. */
typedef struct trees_pin_Pin_stamps_frame_0_t
{
	TreesPinPin *self;
	HostIoIo   *io;
	TreesStoreNamed tree;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	StdArcInner **result;
	ChunksErrorError *error;
	bool		ok;
} trees_pin_Pin_stamps_frame_0_t;

/* trees_pin_Pin_stamps_body_0 is stamps' body. */
static void
trees_pin_Pin_stamps_body_0(void *frame_)
{
	trees_pin_Pin_stamps_frame_0_t *frame = frame_;
	TreesNodeNode node;

	if (!trees_pin_Pin_node(frame->self, frame->io, frame->tree, &node, frame->error))
	{
		frame->ok = false;
		return;
	}
	trees_node_Node_stamps(&node, &frame->stamps);
	frame->stamps_live = true;
	*frame->result = std_arc_new(&frame->stamps, sizeof(ChunksTrailerStamps), _Alignof(ChunksTrailerStamps));
	frame->stamps_live = false;
	frame->ok = true;
}

/* trees_pin_Pin_stamps_pad_0 is stamps' pad: the stamps, while it holds them. */
static void
trees_pin_Pin_stamps_pad_0(void *frame_)
{
	trees_pin_Pin_stamps_frame_0_t *frame = frame_;

	if (frame->stamps_live)
		chunks_trailer_Stamps_drop(&frame->stamps);
}

/* trees_pin_Pin_stamps returns the row stamps of the leaf a tree name names, which its node holds, in an Arc<Stamps>. */
bool
trees_pin_Pin_stamps(TreesPinPin *self, HostIoIo *io, TreesStoreNamed tree, StdArcInner **result,
					 ChunksErrorError *error)
{
	trees_pin_Pin_stamps_frame_0_t frame = {.self = self, .io = io, .tree = tree, .result = result, .error = error};

	std_landing(&frame, trees_pin_Pin_stamps_body_0, trees_pin_Pin_stamps_pad_0);
	return frame.ok;
}
