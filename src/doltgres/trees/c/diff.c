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
 * The C port of trees' diff.rs: diffs between two trees that skip every
 * subtree the two share, and the three-way walk of the keys two trees changed
 * from their base.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/address.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/diff.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/pin.h"

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "diff.rs"

/* trees_TupleCursorCursor_t is (Cursor, Cursor): the cursors at the first entries of the two trees. */
typedef struct trees_TupleCursorCursor_t
{
	TreesCursorCursor f0;
	TreesCursorCursor f1;
} trees_TupleCursorCursor_t;

#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultTupleCursorCursor
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , trees_TupleCursorCursor_t *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS trees_TupleCursorCursor_t *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

#define TREES_PIN_SCOPE extern
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultBool
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS bool *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

static bool trees_diff_skip_shared(HostIoIo *io, TreesPinPin *pin, TreesCursorCursor *a, TreesCursorCursor *b,
								   bool *result, ChunksErrorError *error);

/* trees_diff_SliceU8_PartialEqSliceU8_eq is [u8]'s PartialEq: the same bytes. */
bool
trees_diff_SliceU8_PartialEqSliceU8_eq(const StdBytes *self, const StdBytes *other)
{
	return self->len == other->len && (self->len == 0 || memcmp(self->ptr, other->ptr, self->len) == 0);
}

/* trees_diff_SliceU8_PartialEqSliceU8_ne is [u8]'s PartialEq::ne: eq's opposite. */
static bool
trees_diff_SliceU8_PartialEqSliceU8_ne(const StdBytes *self, const StdBytes *other)
{
	return !trees_diff_SliceU8_PartialEqSliceU8_eq(self, other);
}

/*
 * trees_diff_Change_Clone_clone_frame_0_t is Change's Clone's state across
 * its pad: the first address list cloned, while live.
 */
typedef struct trees_diff_Change_Clone_clone_frame_0_t
{
	const TreesDiffChange *self;
	TreesDiffChange *result;
	bool		from_addresses_live;
} trees_diff_Change_Clone_clone_frame_0_t;

/* trees_diff_Change_Clone_clone_body_0 is Change's derived Clone's body: the variant's fields cloned in order. */
static void
trees_diff_Change_Clone_clone_body_0(void *frame_)
{
	trees_diff_Change_Clone_clone_frame_0_t *frame = frame_;
	const TreesDiffChange *self = frame->self;
	TreesDiffChange *result = frame->result;

	result->tag = self->tag;
	switch (self->tag)
	{
		case TREES_DIFF_CHANGE_ADDED:
			result->u.added.key = self->u.added.key;
			result->u.added.to = self->u.added.to;
			std_vec_clone(&result->u.added.addresses, &self->u.added.addresses, sizeof(chunks_TupleU32Locator_t),
						  _Alignof(chunks_TupleU32Locator_t));
			break;
		case TREES_DIFF_CHANGE_REMOVED:
			result->u.removed.key = self->u.removed.key;
			result->u.removed.from = self->u.removed.from;
			std_vec_clone(&result->u.removed.from_addresses, &self->u.removed.from_addresses,
						  sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
			break;
		case TREES_DIFF_CHANGE_MODIFIED:
			result->u.modified.key = self->u.modified.key;
			result->u.modified.from = self->u.modified.from;
			std_vec_clone(&result->u.modified.from_addresses, &self->u.modified.from_addresses,
						  sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
			frame->from_addresses_live = true;
			result->u.modified.to = self->u.modified.to;
			std_vec_clone(&result->u.modified.addresses, &self->u.modified.addresses,
						  sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
			frame->from_addresses_live = false;
			break;
	}
}

/* trees_diff_Change_Clone_clone_pad_0 is Change's Clone's pad: the first address list cloned, while live. */
static void
trees_diff_Change_Clone_clone_pad_0(void *frame_)
{
	trees_diff_Change_Clone_clone_frame_0_t *frame = frame_;

	if (frame->from_addresses_live)
		std_vec_free_Checked(&frame->result->u.modified.from_addresses, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
}

/* trees_diff_Change_Clone_clone is Change's derived Clone. */
void
trees_diff_Change_Clone_clone(const void *self_, void *result_)
{
	trees_diff_Change_Clone_clone_frame_0_t frame = {.self = self_, .result = result_};

	std_landing(&frame, trees_diff_Change_Clone_clone_body_0, trees_diff_Change_Clone_clone_pad_0);
}

/* trees_diff_Change_Debug_fmt is Change's derived Debug: the variant's name and its fields. */
bool
trees_diff_Change_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesDiffChange *self = value;
	StdDebugStruct s;

	switch (self->tag)
	{
		case TREES_DIFF_CHANGE_ADDED:
			s = std_fmt_debug_struct(f, "Added", 5);
			std_DebugStruct_field(&s, "key", 3, &self->u.added.key, std_bytes_Debug_fmt);
			std_DebugStruct_field(&s, "to", 2, &self->u.added.to, std_bytes_Debug_fmt);
			std_DebugStruct_field(&s, "addresses", 9, &self->u.added.addresses,
								  trees_node_VecTupleU32Locator_Debug_fmt);
			break;
		case TREES_DIFF_CHANGE_REMOVED:
			s = std_fmt_debug_struct(f, "Removed", 7);
			std_DebugStruct_field(&s, "key", 3, &self->u.removed.key, std_bytes_Debug_fmt);
			std_DebugStruct_field(&s, "from", 4, &self->u.removed.from, std_bytes_Debug_fmt);
			std_DebugStruct_field(&s, "from_addresses", 14, &self->u.removed.from_addresses,
								  trees_node_VecTupleU32Locator_Debug_fmt);
			break;
		case TREES_DIFF_CHANGE_MODIFIED:
		default:
			s = std_fmt_debug_struct(f, "Modified", 8);
			std_DebugStruct_field(&s, "key", 3, &self->u.modified.key, std_bytes_Debug_fmt);
			std_DebugStruct_field(&s, "from", 4, &self->u.modified.from, std_bytes_Debug_fmt);
			std_DebugStruct_field(&s, "from_addresses", 14, &self->u.modified.from_addresses,
								  trees_node_VecTupleU32Locator_Debug_fmt);
			std_DebugStruct_field(&s, "to", 2, &self->u.modified.to, std_bytes_Debug_fmt);
			std_DebugStruct_field(&s, "addresses", 9, &self->u.modified.addresses,
								  trees_node_VecTupleU32Locator_Debug_fmt);
			break;
	}
	return std_DebugStruct_finish(&s);
}

/* trees_diff_Change_PartialEq_eq is Change's derived PartialEq: the same variant with its fields equal in order. */
bool
trees_diff_Change_PartialEq_eq(const void *self_, const void *other_)
{
	const TreesDiffChange *self = self_;
	const TreesDiffChange *other = other_;

	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case TREES_DIFF_CHANGE_ADDED:
			return trees_diff_SliceU8_PartialEqSliceU8_eq(&self->u.added.key, &other->u.added.key) &&
				trees_diff_SliceU8_PartialEqSliceU8_eq(&self->u.added.to, &other->u.added.to) &&
				trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq(&self->u.added.addresses,
																			 &other->u.added.addresses);
		case TREES_DIFF_CHANGE_REMOVED:
			return trees_diff_SliceU8_PartialEqSliceU8_eq(&self->u.removed.key, &other->u.removed.key) &&
				trees_diff_SliceU8_PartialEqSliceU8_eq(&self->u.removed.from, &other->u.removed.from) &&
				trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq(&self->u.removed.from_addresses,
																			 &other->u.removed.from_addresses);
		case TREES_DIFF_CHANGE_MODIFIED:
		default:
			return trees_diff_SliceU8_PartialEqSliceU8_eq(&self->u.modified.key, &other->u.modified.key) &&
				trees_diff_SliceU8_PartialEqSliceU8_eq(&self->u.modified.from, &other->u.modified.from) &&
				trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq(&self->u.modified.from_addresses,
																			 &other->u.modified.from_addresses) &&
				trees_diff_SliceU8_PartialEqSliceU8_eq(&self->u.modified.to, &other->u.modified.to) &&
				trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq(&self->u.modified.addresses,
																			 &other->u.modified.addresses);
	}
}

/* trees_diff_Change_drop is Change's drop glue: the variant's address lists. */
void
trees_diff_Change_drop(void *self_)
{
	TreesDiffChange *self = self_;

	switch (self->tag)
	{
		case TREES_DIFF_CHANGE_ADDED:
			std_vec_free_Checked(&self->u.added.addresses, sizeof(chunks_TupleU32Locator_t),
								 _Alignof(chunks_TupleU32Locator_t));
			break;
		case TREES_DIFF_CHANGE_REMOVED:
			std_vec_free_Checked(&self->u.removed.from_addresses, sizeof(chunks_TupleU32Locator_t),
								 _Alignof(chunks_TupleU32Locator_t));
			break;
		case TREES_DIFF_CHANGE_MODIFIED:
			std_vec_free_Checked(&self->u.modified.from_addresses, sizeof(chunks_TupleU32Locator_t),
								 _Alignof(chunks_TupleU32Locator_t));
			std_vec_free_Checked(&self->u.modified.addresses, sizeof(chunks_TupleU32Locator_t),
								 _Alignof(chunks_TupleU32Locator_t));
			break;
	}
}

/* trees_diff_Change_key returns the key the change is to. */
StdBytes
trees_diff_Change_key(const TreesDiffChange *self)
{
	switch (self->tag)
	{
		case TREES_DIFF_CHANGE_ADDED:
			return self->u.added.key;
		case TREES_DIFF_CHANGE_REMOVED:
			return self->u.removed.key;
		case TREES_DIFF_CHANGE_MODIFIED:
		default:
			return self->u.modified.key;
	}
}

/* trees_diff_Change_from returns the value before the change, none (a NULL pointer) for an addition. */
StdBytes
trees_diff_Change_from(const TreesDiffChange *self)
{
	StdBytes	none = {NULL, 0};

	switch (self->tag)
	{
		case TREES_DIFF_CHANGE_ADDED:
			return none;
		case TREES_DIFF_CHANGE_REMOVED:
			return self->u.removed.from;
		case TREES_DIFF_CHANGE_MODIFIED:
		default:
			return self->u.modified.from;
	}
}

/* trees_diff_Change_to returns the value after the change, none (a NULL pointer) for a removal. */
StdBytes
trees_diff_Change_to(const TreesDiffChange *self)
{
	StdBytes	none = {NULL, 0};

	switch (self->tag)
	{
		case TREES_DIFF_CHANGE_REMOVED:
			return none;
		case TREES_DIFF_CHANGE_ADDED:
			return self->u.added.to;
		case TREES_DIFF_CHANGE_MODIFIED:
		default:
			return self->u.modified.to;
	}
}

/* trees_diff_diff_frame_0_t is diff's state across its pad: the cursor and the function's captures, while live. */
typedef struct trees_diff_diff_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreNamed from;
	TreesStoreNamed to;
	const TreesKeyOrder *order;
	bool		(*f) (void *captures, HostIoIo *io, TreesDiffChange *change, bool *result, ChunksErrorError *error);
	void	   *f_captures;
	void		(*f_drop) (void *captures);
	bool		f_live;
	ChunksErrorError *error;
	bool		ok;
	TreesDiffDiffCursor cursor;
	bool		cursor_live;
} trees_diff_diff_frame_0_t;

/* trees_diff_diff_pad_0 is diff's pad: the cursor, then the function's captures, while live. */
static void
trees_diff_diff_pad_0(void *frame_)
{
	trees_diff_diff_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_diff_DiffCursor_drop(&frame->cursor);
	if (frame->f_live && frame->f_drop != NULL)
		frame->f_drop(frame->f_captures);
}

/* trees_diff_diff_body_0 is diff's body: each change handed to the function until it returns false or none is left. */
static void
trees_diff_diff_body_0(void *frame_)
{
	trees_diff_diff_frame_0_t *frame = frame_;
	TreesStoreTree from = trees_store_name(frame->from);
	TreesStoreTree to = trees_store_name(frame->to);

	frame->ok = false;
	if (!trees_diff_DiffCursor_new(frame->io, frame->store, TREES_STORE_TREE_NAMED(&from),
								   TREES_STORE_TREE_NAMED(&to), frame->order, &frame->cursor, frame->error))
	{
		trees_diff_diff_pad_0(frame);
		return;
	}
	frame->cursor_live = true;
	for (;;)
	{
		bool		some;
		TreesDiffChange change;
		bool		more;

		trees_diff_DiffCursor_change(&frame->cursor, &some, &change);
		if (!some)
			break;
		if (!frame->f(frame->f_captures, frame->io, &change, &more, frame->error))
		{
			trees_diff_diff_pad_0(frame);
			return;
		}
		if (!more)
		{
			frame->ok = true;
			trees_diff_diff_pad_0(frame);
			return;
		}
		if (!trees_diff_DiffCursor_advance(&frame->cursor, frame->io, frame->error))
		{
			trees_diff_diff_pad_0(frame);
			return;
		}
	}
	frame->ok = true;
	trees_diff_diff_pad_0(frame);
}

/*
 * trees_diff_diff calls the function with each change from one tree to the
 * other, in the trees' key order, until it returns false; the function takes
 * each change by value.
 */
bool
trees_diff_diff(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed from, TreesStoreNamed to,
				const TreesKeyOrder *order,
				bool (*f) (void *captures, HostIoIo *io, TreesDiffChange *change, bool *result,
						   ChunksErrorError *error),
				void *f_captures, void (*f_drop) (void *captures), ChunksErrorError *error)
{
	trees_diff_diff_frame_0_t frame = {.io = io, .store = store, .from = from, .to = to, .order = order, .f = f,
	.f_captures = f_captures, .f_drop = f_drop, .f_live = true, .error = error};

	std_landing(&frame, trees_diff_diff_body_0, trees_diff_diff_pad_0);
	return frame.ok;
}

/* trees_diff_DiffCursor_new_closure_0_captures_t is new's closure's captures: the two trees. */
typedef struct trees_diff_DiffCursor_new_closure_0_captures_t
{
	TreesStoreTree from;
	TreesStoreTree to;
} trees_diff_DiffCursor_new_closure_0_captures_t;

/*
 * trees_diff_DiffCursor_new_closure_0_frame_0_t is new's closure's state
 * across its pad: the first cursor, while live.
 */
typedef struct trees_diff_DiffCursor_new_closure_0_frame_0_t
{
	const trees_diff_DiffCursor_new_closure_0_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	trees_TupleCursorCursor_t *result;
	ChunksErrorError *error;
	bool		ok;
	bool		first_live;
} trees_diff_DiffCursor_new_closure_0_frame_0_t;

/* trees_diff_DiffCursor_new_closure_0_pad_0 is new's closure's pad: the first cursor, while live. */
static void
trees_diff_DiffCursor_new_closure_0_pad_0(void *frame_)
{
	trees_diff_DiffCursor_new_closure_0_frame_0_t *frame = frame_;

	if (frame->first_live)
		trees_cursor_Cursor_drop(&frame->result->f0);
}

/* trees_diff_DiffCursor_new_closure_0_body_0 is new's closure's body: a cursor at each tree's first entry. */
static void
trees_diff_DiffCursor_new_closure_0_body_0(void *frame_)
{
	trees_diff_DiffCursor_new_closure_0_frame_0_t *frame = frame_;
	TreesStoreTree from = frame->captures->from;
	TreesStoreTree to = frame->captures->to;

	if (!trees_cursor_Cursor_first(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&from), &frame->result->f0,
								   frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->first_live = true;
	if (!trees_cursor_Cursor_first(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&to), &frame->result->f1,
								   frame->error))
	{
		frame->first_live = false;
		trees_cursor_Cursor_drop(&frame->result->f0);
		frame->ok = false;
		return;
	}
	frame->first_live = false;
	frame->ok = true;
}

/* trees_diff_DiffCursor_new_closure_0 is new's closure in its pin: the cursors at the two trees' first entries. */
static bool
trees_diff_DiffCursor_new_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, trees_TupleCursorCursor_t *result,
									ChunksErrorError *error)
{
	trees_diff_DiffCursor_new_closure_0_frame_0_t frame = {.captures = captures, .io = io, .pin = pin,
	.result = result, .error = error};

	std_landing(&frame, trees_diff_DiffCursor_new_closure_0_body_0, trees_diff_DiffCursor_new_closure_0_pad_0);
	return frame.ok;
}

static bool trees_diff_DiffCursor_seek(TreesDiffDiffCursor *self, HostIoIo *io, ChunksErrorError *error);

/* trees_diff_DiffCursor_new_frame_0_t is new's state across its pad: the cursor made, while live. */
typedef struct trees_diff_DiffCursor_new_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreNamed from;
	TreesStoreNamed to;
	const TreesKeyOrder *order;
	TreesDiffDiffCursor *result;
	ChunksErrorError *error;
	bool		ok;
	TreesDiffDiffCursor cursor;
	bool		cursor_live;
} trees_diff_DiffCursor_new_frame_0_t;

/* trees_diff_DiffCursor_new_pad_0 is new's pad: the cursor made, while live. */
static void
trees_diff_DiffCursor_new_pad_0(void *frame_)
{
	trees_diff_DiffCursor_new_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_diff_DiffCursor_drop(&frame->cursor);
}

/* trees_diff_DiffCursor_new_body_0 is new's body: the two trees' first cursors in one pin, then the first change sought. */
static void
trees_diff_DiffCursor_new_body_0(void *frame_)
{
	trees_diff_DiffCursor_new_frame_0_t *frame = frame_;
	trees_diff_DiffCursor_new_closure_0_captures_t trees = {trees_store_name(frame->from), trees_store_name(frame->to)};
	TreesStoreNodeStore store = frame->store.ops->as_dyn(frame->store.self);
	trees_TupleCursorCursor_t cursors;

	if (!trees_pin_pin_ResultTupleCursorCursor(frame->io, store, trees_diff_DiffCursor_new_closure_0, &trees, NULL,
											   &cursors, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->cursor.store = store;
	frame->cursor.order = frame->order;
	frame->cursor.from = cursors.f0;
	frame->cursor.to = cursors.f1;
	frame->cursor.at = STD_ORDERING_EQUAL;
	frame->cursor_live = true;
	if (!trees_diff_DiffCursor_seek(&frame->cursor, frame->io, frame->error))
	{
		frame->cursor_live = false;
		trees_diff_DiffCursor_drop(&frame->cursor);
		frame->ok = false;
		return;
	}
	frame->cursor_live = false;
	*frame->result = frame->cursor;
	frame->ok = true;
}

/* trees_diff_DiffCursor_new returns a cursor at the first change from one tree to the other. */
bool
trees_diff_DiffCursor_new(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed from, TreesStoreNamed to,
						  const TreesKeyOrder *order, TreesDiffDiffCursor *result, ChunksErrorError *error)
{
	trees_diff_DiffCursor_new_frame_0_t frame = {.io = io, .store = store, .from = from, .to = to, .order = order,
	.result = result, .error = error};

	std_landing(&frame, trees_diff_DiffCursor_new_body_0, trees_diff_DiffCursor_new_pad_0);
	return frame.ok;
}

/*
 * trees_diff_DiffCursor_change_frame_0_t is change's state across its pad: a
 * modification's old address list, while live.
 */
typedef struct trees_diff_DiffCursor_change_frame_0_t
{
	const TreesDiffDiffCursor *self;
	bool	   *some;
	TreesDiffChange *result;
	bool		from_addresses_live;
} trees_diff_DiffCursor_change_frame_0_t;

/* trees_diff_DiffCursor_change_body_0 is change's body: the change the cursors' order says, none past the last. */
static void
trees_diff_DiffCursor_change_body_0(void *frame_)
{
	trees_diff_DiffCursor_change_frame_0_t *frame = frame_;
	const TreesCursorCursor *a = &frame->self->from;
	const TreesCursorCursor *b = &frame->self->to;
	TreesDiffChange *result = frame->result;
	TreesNodeNode node;

	if (!trees_cursor_Cursor_is_valid(a) && !trees_cursor_Cursor_is_valid(b))
	{
		*frame->some = false;
		return;
	}
	switch (frame->self->at)
	{
		case STD_ORDERING_LESS:
			result->tag = TREES_DIFF_CHANGE_REMOVED;
			result->u.removed.key = trees_cursor_Cursor_key(a);
			result->u.removed.from = trees_cursor_Cursor_value(a);
			node = trees_cursor_Cursor_node(a);
			trees_node_Node_value_addresses(&node, trees_cursor_Cursor_index(a), &result->u.removed.from_addresses);
			break;
		case STD_ORDERING_GREATER:
			result->tag = TREES_DIFF_CHANGE_ADDED;
			result->u.added.key = trees_cursor_Cursor_key(b);
			result->u.added.to = trees_cursor_Cursor_value(b);
			node = trees_cursor_Cursor_node(b);
			trees_node_Node_value_addresses(&node, trees_cursor_Cursor_index(b), &result->u.added.addresses);
			break;
		case STD_ORDERING_EQUAL:
			result->tag = TREES_DIFF_CHANGE_MODIFIED;
			result->u.modified.key = trees_cursor_Cursor_key(a);
			result->u.modified.from = trees_cursor_Cursor_value(a);
			node = trees_cursor_Cursor_node(a);
			trees_node_Node_value_addresses(&node, trees_cursor_Cursor_index(a), &result->u.modified.from_addresses);
			frame->from_addresses_live = true;
			result->u.modified.to = trees_cursor_Cursor_value(b);
			node = trees_cursor_Cursor_node(b);
			trees_node_Node_value_addresses(&node, trees_cursor_Cursor_index(b), &result->u.modified.addresses);
			frame->from_addresses_live = false;
			break;
	}
	*frame->some = true;
}

/* trees_diff_DiffCursor_change_pad_0 is change's pad: a modification's old address list, while live. */
static void
trees_diff_DiffCursor_change_pad_0(void *frame_)
{
	trees_diff_DiffCursor_change_frame_0_t *frame = frame_;

	if (frame->from_addresses_live)
		std_vec_free_Checked(&frame->result->u.modified.from_addresses, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
}

/* trees_diff_DiffCursor_change returns the current change, none past the last. */
void
trees_diff_DiffCursor_change(const TreesDiffDiffCursor *self, bool *some, TreesDiffChange *result)
{
	trees_diff_DiffCursor_change_frame_0_t frame = {.self = self, .some = some, .result = result};

	std_landing(&frame, trees_diff_DiffCursor_change_body_0, trees_diff_DiffCursor_change_pad_0);
}

/* trees_diff_DiffCursor_step_closure_0 is step's first closure in its pin: the old tree's cursor moved on. */
static bool
trees_diff_DiffCursor_step_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	return trees_cursor_Cursor_next(captures, io, pin, error);
}

/* trees_diff_DiffCursor_step_closure_1 is step's second closure in its pin: the new tree's cursor moved on. */
static bool
trees_diff_DiffCursor_step_closure_1(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	return trees_cursor_Cursor_next(captures, io, pin, error);
}

/* trees_diff_DiffCursor_step moves the cursors past the entries of the current change. */
static bool
trees_diff_DiffCursor_step(TreesDiffDiffCursor *self, HostIoIo *io, ChunksErrorError *error)
{
	if (self->at != STD_ORDERING_GREATER && trees_cursor_Cursor_is_valid(&self->from))
	{
		if (!trees_pin_pin_ResultUnit(io, self->store, trees_diff_DiffCursor_step_closure_0, &self->from, NULL,
									  error))
			return false;
	}
	if (self->at != STD_ORDERING_LESS && trees_cursor_Cursor_is_valid(&self->to))
	{
		if (!trees_pin_pin_ResultUnit(io, self->store, trees_diff_DiffCursor_step_closure_1, &self->to, NULL, error))
			return false;
	}
	return true;
}

/* trees_diff_DiffCursor_seek_closure_0 is seek's closure in its pin: both cursors past the subtrees they share. */
static bool
trees_diff_DiffCursor_seek_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, bool *result,
									 ChunksErrorError *error)
{
	TreesDiffDiffCursor *self = captures;

	return trees_diff_skip_shared(io, pin, &self->from, &self->to, result, error);
}

/*
 * trees_diff_DiffCursor_seek moves the cursors to the next entries that
 * differ, past the subtrees the trees share and the entries they hold alike.
 */
static bool
trees_diff_DiffCursor_seek(TreesDiffDiffCursor *self, HostIoIo *io, ChunksErrorError *error)
{
	while (trees_cursor_Cursor_is_valid(&self->from) || trees_cursor_Cursor_is_valid(&self->to))
	{
		StdBytes	from;
		StdBytes	to;

		if (trees_cursor_Cursor_is_valid(&self->from) && trees_cursor_Cursor_is_valid(&self->to))
		{
			bool		skipped;

			if (!trees_pin_pin_ResultBool(io, self->store, trees_diff_DiffCursor_seek_closure_0, self, NULL,
										  &skipped, error))
				return false;
			if (skipped)
				continue;
		}
		if (trees_cursor_Cursor_is_valid(&self->from) && trees_cursor_Cursor_is_valid(&self->to))
		{
			StdBytes	a = trees_cursor_Cursor_key(&self->from);
			StdBytes	b = trees_cursor_Cursor_key(&self->to);

			if (!trees_KeyOrder_compare(self->order, io, a.ptr, a.len, b.ptr, b.len, &self->at, error))
				return false;
		}
		else if (trees_cursor_Cursor_is_valid(&self->from))
			self->at = STD_ORDERING_LESS;
		else
			self->at = STD_ORDERING_GREATER;
		if (self->at != STD_ORDERING_EQUAL)
			return true;
		from = trees_cursor_Cursor_value(&self->from);
		to = trees_cursor_Cursor_value(&self->to);
		if (trees_diff_SliceU8_PartialEqSliceU8_ne(&from, &to))
			return true;
		if (!trees_diff_DiffCursor_step(self, io, error))
			return false;
	}
	return true;
}

/* trees_diff_DiffCursor_advance moves to the next change. */
bool
trees_diff_DiffCursor_advance(TreesDiffDiffCursor *self, HostIoIo *io, ChunksErrorError *error)
{
	if (!trees_diff_DiffCursor_step(self, io, error))
		return false;
	return trees_diff_DiffCursor_seek(self, io, error);
}

/* trees_diff_DiffCursor_drop is DiffCursor's drop glue: its two cursors. */
void
trees_diff_DiffCursor_drop(TreesDiffDiffCursor *self)
{
	trees_cursor_Cursor_drop(&self->from);
	trees_cursor_Cursor_drop(&self->to);
}

/* trees_diff_ThreeWay_key returns the key the sides changed. */
StdBytes
trees_diff_ThreeWay_key(const TreesDiffThreeWay *self)
{
	StdBytes	empty = {STD_DANGLING_FOR(1), 0};

	if (self->has_ours)
		return trees_diff_Change_key(&self->ours);
	if (self->has_theirs)
		return trees_diff_Change_key(&self->theirs);
	return empty;
}

/* trees_diff_ThreeWay_base returns the base's value, none (a NULL pointer) when the base has no entry for the key. */
StdBytes
trees_diff_ThreeWay_base(const TreesDiffThreeWay *self)
{
	StdBytes	none = {NULL, 0};

	if (self->has_ours)
		return trees_diff_Change_from(&self->ours);
	if (self->has_theirs)
		return trees_diff_Change_from(&self->theirs);
	return none;
}

/* trees_diff_ThreeWay_ours returns our value, none (a NULL pointer) when we have no entry for the key. */
StdBytes
trees_diff_ThreeWay_ours(const TreesDiffThreeWay *self)
{
	StdBytes	base = trees_diff_ThreeWay_base(self);

	return self->has_ours ? trees_diff_Change_to(&self->ours) : base;
}

/* trees_diff_ThreeWay_theirs returns their value, none (a NULL pointer) when they have no entry for the key. */
StdBytes
trees_diff_ThreeWay_theirs(const TreesDiffThreeWay *self)
{
	StdBytes	base = trees_diff_ThreeWay_base(self);

	return self->has_theirs ? trees_diff_Change_to(&self->theirs) : base;
}

/* trees_diff_ThreeWay_drop is ThreeWay's drop glue: each side's change, if any. */
void
trees_diff_ThreeWay_drop(TreesDiffThreeWay *self)
{
	if (self->has_ours)
		trees_diff_Change_drop(&self->ours);
	if (self->has_theirs)
		trees_diff_Change_drop(&self->theirs);
}

static bool trees_diff_ThreeWayCursor_compare(TreesDiffThreeWayCursor *self, HostIoIo *io, ChunksErrorError *error);

/*
 * trees_diff_ThreeWayCursor_new_frame_0_t is ThreeWayCursor::new's state
 * across its pad: our diff's cursor, then the cursor made, while live.
 */
typedef struct trees_diff_ThreeWayCursor_new_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreNamed base;
	TreesStoreNamed ours;
	TreesStoreNamed theirs;
	const TreesKeyOrder *order;
	TreesDiffThreeWayCursor *result;
	ChunksErrorError *error;
	bool		ok;
	TreesDiffThreeWayCursor cursor;
	bool		ours_live;
	bool		cursor_live;
} trees_diff_ThreeWayCursor_new_frame_0_t;

/* trees_diff_ThreeWayCursor_new_pad_0 is ThreeWayCursor::new's pad: the cursor made, or our diff's alone, while live. */
static void
trees_diff_ThreeWayCursor_new_pad_0(void *frame_)
{
	trees_diff_ThreeWayCursor_new_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_diff_ThreeWayCursor_drop(&frame->cursor);
	if (frame->ours_live)
		trees_diff_DiffCursor_drop(&frame->cursor.ours);
}

/* trees_diff_ThreeWayCursor_new_body_0 is ThreeWayCursor::new's body: the diffs from the base to each side, compared. */
static void
trees_diff_ThreeWayCursor_new_body_0(void *frame_)
{
	trees_diff_ThreeWayCursor_new_frame_0_t *frame = frame_;
	TreesStoreTree base = trees_store_name(frame->base);
	TreesStoreTree ours = trees_store_name(frame->ours);
	TreesStoreTree theirs = trees_store_name(frame->theirs);

	frame->ok = false;
	if (!trees_diff_DiffCursor_new(frame->io, frame->store, TREES_STORE_TREE_NAMED(&base),
								   TREES_STORE_TREE_NAMED(&ours), frame->order, &frame->cursor.ours, frame->error))
		return;
	frame->ours_live = true;
	if (!trees_diff_DiffCursor_new(frame->io, frame->store, TREES_STORE_TREE_NAMED(&base),
								   TREES_STORE_TREE_NAMED(&theirs), frame->order, &frame->cursor.theirs,
								   frame->error))
	{
		frame->ours_live = false;
		trees_diff_DiffCursor_drop(&frame->cursor.ours);
		return;
	}
	frame->ours_live = false;
	frame->cursor.order = frame->order;
	frame->cursor.at = STD_ORDERING_EQUAL;
	frame->cursor_live = true;
	if (!trees_diff_ThreeWayCursor_compare(&frame->cursor, frame->io, frame->error))
	{
		frame->cursor_live = false;
		trees_diff_ThreeWayCursor_drop(&frame->cursor);
		return;
	}
	frame->cursor_live = false;
	*frame->result = frame->cursor;
	frame->ok = true;
}

/* trees_diff_ThreeWayCursor_new returns a cursor at the first key that ours or theirs changed from the base. */
bool
trees_diff_ThreeWayCursor_new(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed base, TreesStoreNamed ours,
							  TreesStoreNamed theirs, const TreesKeyOrder *order, TreesDiffThreeWayCursor *result,
							  ChunksErrorError *error)
{
	trees_diff_ThreeWayCursor_new_frame_0_t frame = {.io = io, .store = store, .base = base, .ours = ours,
	.theirs = theirs, .order = order, .result = result, .error = error};

	std_landing(&frame, trees_diff_ThreeWayCursor_new_body_0, trees_diff_ThreeWayCursor_new_pad_0);
	return frame.ok;
}

/*
 * trees_diff_ThreeWayCursor_change_frame_0_t is ThreeWayCursor::change's
 * state across its pad: our diff's change, while live.
 */
typedef struct trees_diff_ThreeWayCursor_change_frame_0_t
{
	const TreesDiffThreeWayCursor *self;
	bool	   *some;
	TreesDiffThreeWay *result;
	bool		ours_live;
} trees_diff_ThreeWayCursor_change_frame_0_t;

/*
 * trees_diff_ThreeWayCursor_change_body_0 is ThreeWayCursor::change's body:
 * both diffs' changes, kept as the order says and the other dropped.
 */
static void
trees_diff_ThreeWayCursor_change_body_0(void *frame_)
{
	trees_diff_ThreeWayCursor_change_frame_0_t *frame = frame_;
	TreesDiffThreeWay *result = frame->result;

	trees_diff_DiffCursor_change(&frame->self->ours, &result->has_ours, &result->ours);
	frame->ours_live = result->has_ours;
	trees_diff_DiffCursor_change(&frame->self->theirs, &result->has_theirs, &result->theirs);
	frame->ours_live = false;
	if (!result->has_ours && !result->has_theirs)
	{
		*frame->some = false;
		return;
	}
	switch (frame->self->at)
	{
		case STD_ORDERING_LESS:
			if (result->has_theirs)
				trees_diff_Change_drop(&result->theirs);
			result->has_theirs = false;
			break;
		case STD_ORDERING_GREATER:
			if (result->has_ours)
				trees_diff_Change_drop(&result->ours);
			result->has_ours = false;
			break;
		case STD_ORDERING_EQUAL:
			break;
	}
	*frame->some = true;
}

/* trees_diff_ThreeWayCursor_change_pad_0 is ThreeWayCursor::change's pad: our diff's change, while live. */
static void
trees_diff_ThreeWayCursor_change_pad_0(void *frame_)
{
	trees_diff_ThreeWayCursor_change_frame_0_t *frame = frame_;

	if (frame->ours_live)
		trees_diff_Change_drop(&frame->result->ours);
}

/* trees_diff_ThreeWayCursor_change returns the current key's changes, none past the last. */
void
trees_diff_ThreeWayCursor_change(const TreesDiffThreeWayCursor *self, bool *some, TreesDiffThreeWay *result)
{
	trees_diff_ThreeWayCursor_change_frame_0_t frame = {.self = self, .some = some, .result = result};

	std_landing(&frame, trees_diff_ThreeWayCursor_change_body_0, trees_diff_ThreeWayCursor_change_pad_0);
}

/* trees_diff_ThreeWayCursor_advance moves to the next key either side changed. */
bool
trees_diff_ThreeWayCursor_advance(TreesDiffThreeWayCursor *self, HostIoIo *io, ChunksErrorError *error)
{
	bool		some;
	TreesDiffChange change;

	if (self->at != STD_ORDERING_GREATER)
	{
		trees_diff_DiffCursor_change(&self->ours, &some, &change);
		if (some)
			trees_diff_Change_drop(&change);
		if (some && !trees_diff_DiffCursor_advance(&self->ours, io, error))
			return false;
	}
	if (self->at != STD_ORDERING_LESS)
	{
		trees_diff_DiffCursor_change(&self->theirs, &some, &change);
		if (some)
			trees_diff_Change_drop(&change);
		if (some && !trees_diff_DiffCursor_advance(&self->theirs, io, error))
			return false;
	}
	return trees_diff_ThreeWayCursor_compare(self, io, error);
}

/*
 * trees_diff_ThreeWayCursor_compare_frame_0_t is compare's state across its
 * pad: the two diffs' changes, while live.
 */
typedef struct trees_diff_ThreeWayCursor_compare_frame_0_t
{
	TreesDiffThreeWayCursor *self;
	HostIoIo   *io;
	ChunksErrorError *error;
	bool		ok;
	TreesDiffChange ours;
	bool		ours_live;
	TreesDiffChange theirs;
	bool		theirs_live;
} trees_diff_ThreeWayCursor_compare_frame_0_t;

/* trees_diff_ThreeWayCursor_compare_pad_0 is compare's pad: the pair of changes, ours then theirs, while live. */
static void
trees_diff_ThreeWayCursor_compare_pad_0(void *frame_)
{
	trees_diff_ThreeWayCursor_compare_frame_0_t *frame = frame_;

	if (frame->ours_live)
		trees_diff_Change_drop(&frame->ours);
	if (frame->theirs_live)
		trees_diff_Change_drop(&frame->theirs);
}

/* trees_diff_ThreeWayCursor_compare_body_0 is compare's body: the order of both diffs' current keys. */
static void
trees_diff_ThreeWayCursor_compare_body_0(void *frame_)
{
	trees_diff_ThreeWayCursor_compare_frame_0_t *frame = frame_;
	TreesDiffThreeWayCursor *self = frame->self;

	trees_diff_DiffCursor_change(&self->ours, &frame->ours_live, &frame->ours);
	trees_diff_DiffCursor_change(&self->theirs, &frame->theirs_live, &frame->theirs);
	if (frame->ours_live && frame->theirs_live)
	{
		StdBytes	a = trees_diff_Change_key(&frame->ours);
		StdBytes	b = trees_diff_Change_key(&frame->theirs);

		if (!trees_KeyOrder_compare(self->order, frame->io, a.ptr, a.len, b.ptr, b.len, &self->at, frame->error))
		{
			trees_diff_ThreeWayCursor_compare_pad_0(frame);
			frame->ok = false;
			return;
		}
	}
	else if (frame->ours_live)
		self->at = STD_ORDERING_LESS;
	else
		self->at = STD_ORDERING_GREATER;
	trees_diff_ThreeWayCursor_compare_pad_0(frame);
	frame->ok = true;
}

/* trees_diff_ThreeWayCursor_compare sets how the keys of the two diffs' current changes compare. */
static bool
trees_diff_ThreeWayCursor_compare(TreesDiffThreeWayCursor *self, HostIoIo *io, ChunksErrorError *error)
{
	trees_diff_ThreeWayCursor_compare_frame_0_t frame = {.self = self, .io = io, .error = error};

	std_landing(&frame, trees_diff_ThreeWayCursor_compare_body_0, trees_diff_ThreeWayCursor_compare_pad_0);
	return frame.ok;
}

/* trees_diff_ThreeWayCursor_drop is ThreeWayCursor's drop glue: our diff's cursor, then theirs. */
void
trees_diff_ThreeWayCursor_drop(TreesDiffThreeWayCursor *self)
{
	trees_diff_DiffCursor_drop(&self->ours);
	trees_diff_DiffCursor_drop(&self->theirs);
}

/*
 * trees_diff_skip_shared moves both cursors past the highest node they both
 * start and that the two trees share, and reports whether it moved them.
 */
static bool
trees_diff_skip_shared(HostIoIo *io, TreesPinPin *pin, TreesCursorCursor *a, TreesCursorCursor *b, bool *result,
					   ChunksErrorError *error)
{
	uint8		level = 0;
	bool		has_shared = false;
	uint8		shared = 0;

	if (trees_cursor_Cursor_index(a) != 0 || trees_cursor_Cursor_index(b) != 0)
	{
		*result = false;
		return true;
	}
	while (trees_cursor_Cursor_starts_node(a, level) && trees_cursor_Cursor_starts_node(b, level))
	{
		TreesStoreTree x;
		TreesStoreTree y;

		if (!trees_cursor_Cursor_tree_at(a, level, &x))
			STD_OPTION_UNWRAP_FAILED(294, 40);
		if (!trees_cursor_Cursor_tree_at(b, level, &y))
			STD_OPTION_UNWRAP_FAILED(294, 67);
		if (chunks_address_Address_PartialEq_eq(&x.address, &y.address))
		{
			has_shared = true;
			shared = level;
		}
		level = STD_ADD(u8, level, 1, 298, 9);
	}
	if (!has_shared)
	{
		*result = false;
		return true;
	}
	if (!trees_cursor_Cursor_skip_node(a, io, pin, shared, error))
		return false;
	if (!trees_cursor_Cursor_skip_node(b, io, pin, shared, error))
		return false;
	*result = true;
	return true;
}
