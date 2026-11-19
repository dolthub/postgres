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
 * Prolly trees (trees' lib.rs): search trees of chunks whose node boundaries
 * depend only on the keys they hold. So far the order of a tree's keys and
 * the summarizer of its subtrees, each by bytes or a summary kind's function,
 * or by an engine's functions through an Arc<dyn> that runs under the locks
 * the thread holds or only with its Io.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/diff.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/pin.h"

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "lib.rs"

/* TREES_NODE_BATCH is NODE_BATCH, how many internal nodes node_counts reads in one pin scope. */
#define TREES_NODE_BATCH ((size_t) 256)

/* core's shared sort parts at usize, which the stable sort of positions calls. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS Usize
#define STD_SORT_T size_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

/* The stable sort_by of positions, which sorted_positions sorts by their keys' bytes. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS Usize
#define STD_STABLE_SORT_T size_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY
#include "doltgres/std/stable_sort_template.h"

/* pin's instance at get's closure, R = Result<Option<Vec<u8>>>. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionVecU8
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *some, StdVec *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS bool *some; StdVec *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .some = some, .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->some, frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* pin's instance at count's closure, R = Result<u64>. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultU64
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , uint64 *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS uint64 *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* pin's instance at height's closure, R = Result<u8>. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultU8
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , uint8 *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS uint8 *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/*
 * trees_Summarizer_same_definition reports whether another summarizer is an
 * engine's of the same definition as this one, which a summary kind's
 * function never is, since its kind tells it apart.
 */
bool
trees_Summarizer_same_definition(const TreesSummarizer *self, const TreesSummarizer *other)
{
	if (self->tag == TREES_SUMMARIZER_DYN && other->tag == TREES_SUMMARIZER_DYN)
		return self->u.dyn.ops->same_definition(STD_ARC_VALUE(self->u.dyn.inner, self->u.dyn.ops->type->align),
												other->u.dyn.ops,
												STD_ARC_VALUE(other->u.dyn.inner, other->u.dyn.ops->type->align));
	if (self->tag == TREES_SUMMARIZER_DYN_IO && other->tag == TREES_SUMMARIZER_DYN_IO)
		return self->u.dyn_io.ops->same_definition(STD_ARC_VALUE(self->u.dyn_io.inner,
																 self->u.dyn_io.ops->type->align),
												   other->u.dyn_io.ops,
												   STD_ARC_VALUE(other->u.dyn_io.inner,
																 other->u.dyn_io.ops->type->align));
	return false;
}

/* trees_Summarizer_summarize returns the summary of a node's leaf values at level 0 or its children's summaries above. */
bool
trees_Summarizer_summarize(const TreesSummarizer *self, HostIoIo *io, uint8 level, const StdBytes *items,
						   size_t items_len, StdVec *result, ChunksErrorError *error)
{
	switch (self->tag)
	{
		case TREES_SUMMARIZER_FN:
			return self->u.fn(level, items, items_len, result, error);
		case TREES_SUMMARIZER_DYN:
			return self->u.dyn.ops->summarize(STD_ARC_VALUE(self->u.dyn.inner, self->u.dyn.ops->type->align),
											  host_io_Io_DerefMut_deref_mut(io), level, items, items_len, result,
											  error);
		case TREES_SUMMARIZER_DYN_IO:
			return self->u.dyn_io.ops->summarize(STD_ARC_VALUE(self->u.dyn_io.inner,
															   self->u.dyn_io.ops->type->align),
												 io, level, items, items_len, result, error);
	}
	pg_unreachable();
}

/* trees_Summarizer_functions returns the engine's summarizer as a dyn Any, or false for a summary kind's function. */
bool
trees_Summarizer_functions(const TreesSummarizer *self, StdAny *result)
{
	switch (self->tag)
	{
		case TREES_SUMMARIZER_FN:
			return false;
		case TREES_SUMMARIZER_DYN:
			result->type = self->u.dyn.ops->type;
			result->value = STD_ARC_VALUE(self->u.dyn.inner, self->u.dyn.ops->type->align);
			return true;
		case TREES_SUMMARIZER_DYN_IO:
			result->type = self->u.dyn_io.ops->type;
			result->value = STD_ARC_VALUE(self->u.dyn_io.inner, self->u.dyn_io.ops->type->align);
			return true;
	}
	pg_unreachable();
}

/* trees_Summarizer_held returns the summarizer as code under a lock calls it, or false for one that runs only with the Io. */
bool
trees_Summarizer_held(const TreesSummarizer *self, TreesHeldSummarizer *result)
{
	switch (self->tag)
	{
		case TREES_SUMMARIZER_FN:
			result->tag = TREES_HELD_SUMMARIZER_FN;
			result->u.fn = self->u.fn;
			return true;
		case TREES_SUMMARIZER_DYN:
			result->tag = TREES_HELD_SUMMARIZER_DYN;
			result->u.dyn.ops = self->u.dyn.ops;
			result->u.dyn.self = STD_ARC_VALUE(self->u.dyn.inner, self->u.dyn.ops->type->align);
			return true;
		case TREES_SUMMARIZER_DYN_IO:
			return false;
	}
	pg_unreachable();
}

/* trees_Summarizer_Debug_fmt is Summarizer's Debug: its variant's name. */
bool
trees_Summarizer_Debug_fmt(const void *value, StdFormatter *f)
{
	switch (((const TreesSummarizer *) value)->tag)
	{
		case TREES_SUMMARIZER_FN:
			return std_fmt_write_str(f, "Fn", 2);
		case TREES_SUMMARIZER_DYN:
			return std_fmt_write_str(f, "Dyn", 3);
		case TREES_SUMMARIZER_DYN_IO:
			return std_fmt_write_str(f, "DynIo", 5);
	}
	pg_unreachable();
}

/* trees_Summarizer_Clone_clone is Summarizer's derived Clone: the function copied, or the Arc's strong count raised. */
void
trees_Summarizer_Clone_clone(const TreesSummarizer *self, TreesSummarizer *result)
{
	*result = *self;
	if (self->tag == TREES_SUMMARIZER_DYN)
		(void) std_arc_clone(self->u.dyn.inner);
	else if (self->tag == TREES_SUMMARIZER_DYN_IO)
		(void) std_arc_clone(self->u.dyn_io.inner);
}

/* trees_Summarizer_drop is Summarizer's drop glue: its Arc's drop, the value's through its table. */
void
trees_Summarizer_drop(TreesSummarizer *self)
{
	if (self->tag == TREES_SUMMARIZER_DYN)
		std_arc_drop(self->u.dyn.inner, self->u.dyn.ops->type->size, self->u.dyn.ops->type->align,
					 self->u.dyn.ops->type->drop);
	else if (self->tag == TREES_SUMMARIZER_DYN_IO)
		std_arc_drop(self->u.dyn_io.inner, self->u.dyn_io.ops->type->size, self->u.dyn_io.ops->type->align,
					 self->u.dyn_io.ops->type->drop);
}

/* trees_HeldSummarizer_summarize returns the summary of a node's leaf values at level 0 or its children's summaries above. */
bool
trees_HeldSummarizer_summarize(const TreesHeldSummarizer *self, HostIoHeld *held, uint8 level, const StdBytes *items,
							   size_t items_len, StdVec *result, ChunksErrorError *error)
{
	switch (self->tag)
	{
		case TREES_HELD_SUMMARIZER_FN:
			return self->u.fn(level, items, items_len, result, error);
		case TREES_HELD_SUMMARIZER_DYN:
			return self->u.dyn.ops->summarize(self->u.dyn.self, held, level, items, items_len, result, error);
	}
	pg_unreachable();
}

/* trees_KeyOrder_compare returns how two keys order: by their bytes, or by the comparator. */
bool
trees_KeyOrder_compare(const TreesKeyOrder *self, HostIoIo *io, const uint8 *a, size_t a_len, const uint8 *b,
					   size_t b_len, StdOrdering *result, ChunksErrorError *error)
{
	switch (self->tag)
	{
		case TREES_KEY_ORDER_BYTES:
			*result = std_bytes_Ord_cmp(a, a_len, b, b_len);
			return true;
		case TREES_KEY_ORDER_COMPARE:
			return self->u.compare.ops->compare(STD_ARC_VALUE(self->u.compare.inner,
															  self->u.compare.ops->type->align),
												host_io_Io_DerefMut_deref_mut(io), a, a_len, b, b_len, result,
												error);
		case TREES_KEY_ORDER_COMPARE_IO:
			return self->u.compare_io.ops->compare(STD_ARC_VALUE(self->u.compare_io.inner,
																 self->u.compare_io.ops->type->align),
												   io, a, a_len, b, b_len, result, error);
	}
	pg_unreachable();
}

/* trees_KeyOrder_held returns the order as code under a lock compares by it, or false for one that runs only with the Io. */
bool
trees_KeyOrder_held(const TreesKeyOrder *self, TreesHeldKeyOrder *result)
{
	switch (self->tag)
	{
		case TREES_KEY_ORDER_BYTES:
			result->f0.ops = NULL;
			result->f0.self = NULL;
			return true;
		case TREES_KEY_ORDER_COMPARE:
			result->f0.ops = self->u.compare.ops;
			result->f0.self = STD_ARC_VALUE(self->u.compare.inner, self->u.compare.ops->type->align);
			return true;
		case TREES_KEY_ORDER_COMPARE_IO:
			return false;
	}
	pg_unreachable();
}

/* trees_KeyOrder_functions returns the comparator the order compares by as a dyn Any, or false for byte order. */
bool
trees_KeyOrder_functions(const TreesKeyOrder *self, StdAny *result)
{
	switch (self->tag)
	{
		case TREES_KEY_ORDER_BYTES:
			return false;
		case TREES_KEY_ORDER_COMPARE:
			result->type = self->u.compare.ops->type;
			result->value = STD_ARC_VALUE(self->u.compare.inner, self->u.compare.ops->type->align);
			return true;
		case TREES_KEY_ORDER_COMPARE_IO:
			result->type = self->u.compare_io.ops->type;
			result->value = STD_ARC_VALUE(self->u.compare_io.inner, self->u.compare_io.ops->type->align);
			return true;
	}
	pg_unreachable();
}

/* trees_KeyOrder_is_bytes reports whether the order is the keys' byte order. */
bool
trees_KeyOrder_is_bytes(const TreesKeyOrder *self)
{
	return self->tag == TREES_KEY_ORDER_BYTES;
}

/* trees_KeyOrder_Debug_fmt is KeyOrder's Debug: its variant's name. */
bool
trees_KeyOrder_Debug_fmt(const void *value, StdFormatter *f)
{
	switch (((const TreesKeyOrder *) value)->tag)
	{
		case TREES_KEY_ORDER_BYTES:
			return std_fmt_write_str(f, "Bytes", 5);
		case TREES_KEY_ORDER_COMPARE:
			return std_fmt_write_str(f, "Compare", 7);
		case TREES_KEY_ORDER_COMPARE_IO:
			return std_fmt_write_str(f, "CompareIo", 9);
	}
	pg_unreachable();
}

/* trees_KeyOrder_Clone_clone is KeyOrder's derived Clone: the comparator's Arc's strong count raised. */
void
trees_KeyOrder_Clone_clone(const TreesKeyOrder *self, TreesKeyOrder *result)
{
	*result = *self;
	if (self->tag == TREES_KEY_ORDER_COMPARE)
		(void) std_arc_clone(self->u.compare.inner);
	else if (self->tag == TREES_KEY_ORDER_COMPARE_IO)
		(void) std_arc_clone(self->u.compare_io.inner);
}

/* trees_KeyOrder_Default_default is KeyOrder's derived Default: Bytes. */
TreesKeyOrder
trees_KeyOrder_Default_default(void)
{
	TreesKeyOrder result = {TREES_KEY_ORDER_BYTES};

	return result;
}

/* trees_KeyOrder_drop is KeyOrder's drop glue: its comparator's Arc's drop. */
void
trees_KeyOrder_drop(TreesKeyOrder *self)
{
	if (self->tag == TREES_KEY_ORDER_COMPARE)
		std_arc_drop(self->u.compare.inner, self->u.compare.ops->type->size, self->u.compare.ops->type->align,
					 self->u.compare.ops->type->drop);
	else if (self->tag == TREES_KEY_ORDER_COMPARE_IO)
		std_arc_drop(self->u.compare_io.inner, self->u.compare_io.ops->type->size,
					 self->u.compare_io.ops->type->align, self->u.compare_io.ops->type->drop);
}

/* trees_HeldKeyOrder_compare returns how two keys order: by their bytes, or by the comparator under the held locks. */
bool
trees_HeldKeyOrder_compare(const TreesHeldKeyOrder *self, HostIoHeld *held, const uint8 *a, size_t a_len,
						   const uint8 *b, size_t b_len, StdOrdering *result, ChunksErrorError *error)
{
	if (self->f0.ops == NULL)
	{
		*result = std_bytes_Ord_cmp(a, a_len, b, b_len);
		return true;
	}
	return self->f0.ops->compare(self->f0.self, held, a, a_len, b, b_len, result, error);
}

/* trees_sorted_positions_closure_0_captures_t is sort_by's closure's captures in sorted_positions: the keys, by reference. */
typedef struct trees_sorted_positions_closure_0_captures_t
{
	const StdBytes *keys;
	size_t		keys_len;
} trees_sorted_positions_closure_0_captures_t;

/* trees_sorted_positions_closure_0 is sort_by's closure in sorted_positions: two positions' keys compared as bytes. */
static StdOrdering
trees_sorted_positions_closure_0(void *captures_, const void *a_, const void *b_)
{
	const trees_sorted_positions_closure_0_captures_t *captures = captures_;
	size_t		a = *(const size_t *) a_;
	size_t		b = *(const size_t *) b_;

	std_index_check(a, captures->keys_len, RUST_FILE, 247, 36);
	std_index_check(b, captures->keys_len, RUST_FILE, 247, 48);
	return std_bytes_Ord_cmp(captures->keys[a].ptr, captures->keys[a].len, captures->keys[b].ptr,
							 captures->keys[b].len);
}

/*
 * trees_sorted_positions_frame_0_t is sorted_positions' state across its pad:
 * its arguments, the positions, and the merged run, while live.
 */
typedef struct trees_sorted_positions_frame_0_t
{
	HostIoIo   *io;
	const StdBytes *keys;
	size_t		keys_len;
	const TreesKeyOrder *order;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		positions;
	bool		positions_live;
	StdVec		merged;
	bool		merged_live;
} trees_sorted_positions_frame_0_t;

/* trees_sorted_positions_pad_0 is sorted_positions' pad: the merged run, then the positions, while live. */
static void
trees_sorted_positions_pad_0(void *frame_)
{
	trees_sorted_positions_frame_0_t *frame = frame_;

	if (frame->merged_live)
		std_vec_free_Checked(&frame->merged, sizeof(size_t), _Alignof(size_t));
	if (frame->positions_live)
		std_vec_free_Checked(&frame->positions, sizeof(size_t), _Alignof(size_t));
}

/*
 * trees_sorted_positions_body_0 is sorted_positions' body: the positions
 * sorted by bytes, or merged bottom up by the order's comparisons.
 */
static void
trees_sorted_positions_body_0(void *frame_)
{
	trees_sorted_positions_frame_0_t *frame = frame_;
	const StdBytes *keys = frame->keys;
	size_t		n = frame->keys_len;
	StdSizeHint hint = {n, true, n};
	size_t		width = 1;

	std_vec_with_capacity_trusted(&frame->positions, hint, sizeof(size_t), _Alignof(size_t));
	frame->positions_live = true;
	std_vec_extend_trusted_reserve(&frame->positions, hint, sizeof(size_t), _Alignof(size_t));
	for (size_t k = 0, k_len = n; k < k_len; k++)
	{
		STD_VEC_AT(&frame->positions, size_t, frame->positions.len) = k;
		frame->positions.len = std_wrapping_add_usize(frame->positions.len, 1);
	}
	if (trees_KeyOrder_is_bytes(frame->order))
	{
		trees_sorted_positions_closure_0_captures_t captures = {keys, n};

		std_slice_sort_by_Usize(frame->positions.ptr, frame->positions.len, trees_sorted_positions_closure_0,
								&captures, NULL);
		frame->positions_live = false;
		*frame->result = frame->positions;
		frame->ok = true;
		return;
	}
	std_vec_with_capacity_Checked(&frame->merged, n, sizeof(size_t), _Alignof(size_t));
	frame->merged_live = true;
	while (width < n)
	{
		size_t		step;
		size_t		remaining;
		size_t		next = 0;
		StdVec		swapped;

		std_vec_truncate(&frame->merged, 0, sizeof(size_t), NULL);
		step = STD_MUL(usize, 2, width, 254, 46);
		remaining = std_iter_step_by_range_usize(0, n, step);
		while (remaining > 0)
		{
			size_t		start = next;
			size_t		mid;
			size_t		end;
			size_t		i;
			size_t		j;

			next = std_wrapping_add_usize(start, step);
			remaining = std_wrapping_sub_usize(remaining, 1);
			mid = std_min_usize(STD_ADD(usize, start, width, 255, 31), n);
			end = std_min_usize(STD_ADD(usize, start, STD_MUL(usize, 2, width, 255, 73),
										255, 64), n);
			i = start;
			j = mid;
			while (i < mid && j < end)
			{
				size_t		pj;
				size_t		pi;
				StdOrdering order;
				size_t		taken;

				std_index_check(j, frame->positions.len, RUST_FILE, 258, 52);
				pj = STD_VEC_AT(&frame->positions, size_t, j);
				std_index_check(pj, n, RUST_FILE, 258, 38);
				std_index_check(i, frame->positions.len, RUST_FILE, 258, 72);
				pi = STD_VEC_AT(&frame->positions, size_t, i);
				std_index_check(pi, n, RUST_FILE, 258, 58);
				if (!trees_KeyOrder_compare(frame->order, frame->io, keys[pj].ptr, keys[pj].len, keys[pi].ptr,
											keys[pi].len, &order, frame->error))
				{
					trees_sorted_positions_pad_0(frame);
					frame->ok = false;
					return;
				}
				if (order == STD_ORDERING_LESS)
				{
					std_index_check(j, frame->positions.len, RUST_FILE, 259, 42);
					taken = STD_VEC_AT(&frame->positions, size_t, j);
					std_vec_push_Checked(&frame->merged, &taken, sizeof(size_t), _Alignof(size_t));
					j = STD_ADD(usize, j, 1, 260, 21);
				}
				else
				{
					std_index_check(i, frame->positions.len, RUST_FILE, 262, 42);
					taken = STD_VEC_AT(&frame->positions, size_t, i);
					std_vec_push_Checked(&frame->merged, &taken, sizeof(size_t), _Alignof(size_t));
					i = STD_ADD(usize, i, 1, 263, 21);
				}
			}
			std_range_check(i, mid, frame->positions.len, RUST_FILE, 266, 48);
			std_vec_extend_from_slice_Checked(&frame->merged, STD_PTR_AT(frame->positions.ptr, i, sizeof(size_t)),
											  std_wrapping_sub_usize(mid, i), sizeof(size_t), _Alignof(size_t));
			std_range_check(j, end, frame->positions.len, RUST_FILE, 267, 48);
			std_vec_extend_from_slice_Checked(&frame->merged, STD_PTR_AT(frame->positions.ptr, j, sizeof(size_t)),
											  std_wrapping_sub_usize(end, j), sizeof(size_t), _Alignof(size_t));
		}
		swapped = frame->positions;
		frame->positions = frame->merged;
		frame->merged = swapped;
		width = STD_MUL(usize, width, 2, 270, 9);
	}
	frame->merged_live = false;
	std_vec_free_Checked(&frame->merged, sizeof(size_t), _Alignof(size_t));
	frame->positions_live = false;
	*frame->result = frame->positions;
	frame->ok = true;
}

/*
 * trees_sorted_positions returns the positions of keys in the order's order,
 * stopping at a comparison that fails, as a merge sort does with a comparator
 * that can fail.
 */
bool
trees_sorted_positions(HostIoIo *io, const StdBytes *keys, size_t keys_len, const TreesKeyOrder *order,
					   StdVec *result, ChunksErrorError *error)
{
	trees_sorted_positions_frame_0_t frame = {.io = io, .keys = keys, .keys_len = keys_len, .order = order,
	.result = result, .error = error};

	std_landing(&frame, trees_sorted_positions_body_0, trees_sorted_positions_pad_0);
	return frame.ok;
}

/* trees_Shape_Clone_clone is Shape's derived Clone: its summarizer's clone, if any, then its order's. */
void
trees_Shape_Clone_clone(const TreesShape *self, TreesShape *result)
{
	result->has_summarizer = self->has_summarizer;
	if (self->has_summarizer)
		trees_Summarizer_Clone_clone(&self->summarizer, &result->summarizer);
	trees_KeyOrder_Clone_clone(&self->order, &result->order);
}

/* trees_Shape_Debug_fmt is Shape's derived Debug. */
bool
trees_Shape_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesShape *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Shape", 5);
	StdOptionRef summarizer = {self->has_summarizer, {&self->summarizer, trees_Summarizer_Debug_fmt}};

	std_DebugStruct_field(&s, "summarizer", 10, &summarizer, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "order", 5, &self->order, trees_KeyOrder_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* trees_Shape_Default_default is Shape's derived Default: no summarizer, and bytes' order. */
TreesShape
trees_Shape_Default_default(void)
{
	TreesShape	result = {.order = trees_KeyOrder_Default_default()};

	return result;
}

/* trees_Shape_drop is Shape's drop glue: its summarizer's, if any, then its order's. */
void
trees_Shape_drop(TreesShape *self)
{
	if (self->has_summarizer)
		trees_Summarizer_drop(&self->summarizer);
	trees_KeyOrder_drop(&self->order);
}

/* trees_get_closure_0_captures_t is get's pin closure's captures: the tree and the key. */
typedef struct trees_get_closure_0_captures_t
{
	TreesStoreTree tree;
	const uint8 *key;
	size_t		key_len;
} trees_get_closure_0_captures_t;

/* trees_get_closure_0_frame_0_t is get's pin closure's state across its pad: the cursor, while live. */
typedef struct trees_get_closure_0_frame_0_t
{
	trees_get_closure_0_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	bool	   *some;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	TreesCursorCursor cursor;
	bool		cursor_live;
} trees_get_closure_0_frame_0_t;

/* trees_get_closure_0_pad_0 is get's pin closure's pad: the cursor, while live. */
static void
trees_get_closure_0_pad_0(void *frame_)
{
	trees_get_closure_0_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
}

/* trees_get_closure_0_body_0 is get's pin closure's body: a cursor sought to the key, and its value when it is there. */
static void
trees_get_closure_0_body_0(void *frame_)
{
	trees_get_closure_0_frame_0_t *frame = frame_;
	const TreesKeyOrder bytes = {.tag = TREES_KEY_ORDER_BYTES};
	StdBytes	want = {frame->captures->key, frame->captures->key_len};

	if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&frame->captures->tree),
								  frame->captures->key, frame->captures->key_len, &bytes, &frame->cursor,
								  frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->cursor_live = true;
	*frame->some = false;
	if (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);

		*frame->some = trees_diff_SliceU8_PartialEqSliceU8_eq(&key, &want);
	}
	if (*frame->some)
	{
		StdBytes	value = trees_cursor_Cursor_value(&frame->cursor);

		std_slice_to_vec_Checked(frame->result, value.ptr, value.len, 1, 1);
	}
	frame->cursor_live = false;
	trees_cursor_Cursor_drop(&frame->cursor);
	frame->ok = true;
}

/* trees_get_closure_0 is get's pin closure: the key's value, if the tree holds the key. */
static bool
trees_get_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, bool *some, StdVec *result,
					ChunksErrorError *error)
{
	trees_get_closure_0_frame_0_t frame = {.captures = captures, .io = io, .pin = pin, .some = some,
	.result = result, .error = error};

	std_landing(&frame, trees_get_closure_0_body_0, trees_get_closure_0_pad_0);
	return frame.ok;
}

/* trees_get returns the value of the key in the tree, none when the tree doesn't hold it. */
bool
trees_get(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, const uint8 *key, size_t key_len,
		  bool *some, StdVec *result, ChunksErrorError *error)
{
	trees_get_closure_0_captures_t captures = {trees_store_name(tree), key, key_len};

	return trees_pin_pin_ResultOptionVecU8(io, store.ops->as_dyn(store.self), trees_get_closure_0, &captures, NULL,
										   some, result, error);
}

/* trees_count_closure_0 is count's pin closure: the root's entry total. */
static bool
trees_count_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, uint64 *result, ChunksErrorError *error)
{
	TreesNodeNode node;

	if (!trees_pin_Pin_node(pin, io, TREES_STORE_TREE_NAMED(captures), &node, error))
		return false;
	*result = trees_node_Node_total(&node);
	return true;
}

/* trees_count returns the number of entries in the tree. */
bool
trees_count(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint64 *result, ChunksErrorError *error)
{
	TreesStoreTree named = trees_store_name(tree);

	return trees_pin_pin_ResultU64(io, store.ops->as_dyn(store.self), trees_count_closure_0, &named, NULL, result,
								   error);
}

/* trees_max_tuple_frame_0_t is max_tuple's state across its pad: the root's stamps and the root read, while live. */
typedef struct trees_max_tuple_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreNamed tree;
	uint64	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdArcInner *stamps;
	bool		stamps_live;
	TreesNodeFlat read;
	bool		read_live;
} trees_max_tuple_frame_0_t;

/* trees_max_tuple_pad_0 is max_tuple's pad: the root read, then its stamps, while live. */
static void
trees_max_tuple_pad_0(void *frame_)
{
	trees_max_tuple_frame_0_t *frame = frame_;

	if (frame->read_live)
		trees_node_Flat_drop(&frame->read);
	if (frame->stamps_live)
		trees_cursor_ArcStamps_drop(frame->stamps);
}

/*
 * trees_max_tuple_body_0 is max_tuple's body: the root's stamps' highest
 * tuple id, an internal root's last subtree's checked first.
 */
static void
trees_max_tuple_body_0(void *frame_)
{
	trees_max_tuple_frame_0_t *frame = frame_;
	TreesStoreTree tree = trees_store_name(frame->tree);
	TreesNodeNode node;
	const ChunksTrailerStamps *stamps;

	frame->ok = false;
	if (!trees_store_cached_stamps(frame->io, frame->store, TREES_STORE_TREE_NAMED(&tree), &frame->stamps,
								   frame->error))
		return;
	frame->stamps_live = true;
	stamps = std_arc_Deref_deref(frame->stamps, _Alignof(ChunksTrailerStamps));
	if (!frame->store.ops->read(frame->store.self, frame->io, &tree.address, tree.locator, &frame->read,
								frame->error))
	{
		trees_max_tuple_pad_0(frame);
		return;
	}
	frame->read_live = true;
	node = trees_node_Flat_node(&frame->read);
	if (!trees_node_Node_is_leaf(&node))
	{
		uint64		ignored;

		if (!trees_subtree_max(stamps, STD_SUB(usize, trees_node_Node_len(&node), 1, 305, 30), &ignored,
							   frame->error))
		{
			trees_max_tuple_pad_0(frame);
			return;
		}
	}
	*frame->result = chunks_trailer_Stamps_max_tuple(stamps);
	trees_max_tuple_pad_0(frame);
	frame->ok = true;
}

/* trees_max_tuple returns the highest tuple id a tree's row stamps hold, or 0 for none, which its root's stamps give. */
bool
trees_max_tuple(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint64 *result,
				ChunksErrorError *error)
{
	trees_max_tuple_frame_0_t frame = {.io = io, .store = store, .tree = tree, .result = result, .error = error};

	std_landing(&frame, trees_max_tuple_body_0, trees_max_tuple_pad_0);
	return frame.ok;
}

/* trees_subtree_max returns the highest tuple id of an internal node's child's subtree, failing for a node whose stamps give none. */
bool
trees_subtree_max(const ChunksTrailerStamps *stamps, size_t i, uint64 *result, ChunksErrorError *error)
{
	if (!chunks_trailer_Stamps_subtree_max(stamps, i, result))
	{
		static const char text[] = "internal tree node has no subtree tuple ids";

		*error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
		return false;
	}
	return true;
}

/* trees_leaves returns the number of leaf nodes in the tree. */
bool
trees_leaves(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint64 *result, ChunksErrorError *error)
{
	TreesStoreTree named = trees_store_name(tree);
	std_TupleU64U64_t counts;

	if (!trees_node_counts(io, store, TREES_STORE_TREE_NAMED(&named), &counts, error))
		return false;
	*result = counts.f1;
	return true;
}

/* trees_node_counts_closure_0 is node_counts' first pin closure: whether the root is a leaf. */
static bool
trees_node_counts_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, bool *result, ChunksErrorError *error)
{
	TreesNodeNode node;

	if (!trees_pin_Pin_node(pin, io, TREES_STORE_TREE_NAMED(captures), &node, error))
		return false;
	*result = trees_node_Node_is_leaf(&node);
	return true;
}

/* trees_node_counts_closure_1_captures_t is node_counts' second pin closure's captures: the pending trees and the counts. */
typedef struct trees_node_counts_closure_1_captures_t
{
	StdVec	   *pending;
	uint64	   *internal;
	uint64	   *leaves;
} trees_node_counts_closure_1_captures_t;

/* trees_node_counts_closure_1_frame_0_t is the second pin closure's state across its pad: the batch taken, while live. */
typedef struct trees_node_counts_closure_1_frame_0_t
{
	trees_node_counts_closure_1_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	ChunksErrorError *error;
	bool		ok;
	StdVec		batch;
	bool		batch_live;
} trees_node_counts_closure_1_frame_0_t;

/* trees_node_counts_closure_1_pad_0 is the second pin closure's pad: the batch taken, while live. */
static void
trees_node_counts_closure_1_pad_0(void *frame_)
{
	trees_node_counts_closure_1_frame_0_t *frame = frame_;

	if (frame->batch_live)
		std_vec_free_Checked(&frame->batch, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
}

/*
 * trees_node_counts_closure_1_body_0 is the second pin closure's body: the
 * last batch of pending trees counted, their children pending in turn.
 */
static void
trees_node_counts_closure_1_body_0(void *frame_)
{
	trees_node_counts_closure_1_frame_0_t *frame = frame_;
	trees_node_counts_closure_1_captures_t *captures = frame->captures;
	StdVec	   *pending = captures->pending;

	std_vec_split_off(pending, std_saturating_sub_usize(pending->len, TREES_NODE_BATCH), sizeof(TreesStoreTree),
					  _Alignof(TreesStoreTree), &frame->batch, RUST_FILE, 336, 33);
	frame->batch_live = true;
	for (size_t k = 0, k_len = frame->batch.len; k < k_len; k++)
	{
		TreesStoreTree tree = STD_VEC_AT(&frame->batch, TreesStoreTree, k);
		TreesNodeNode node;
		size_t		n;
		StdSizeHint hint;

		if (!trees_pin_Pin_node(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&tree), &node, frame->error))
		{
			trees_node_counts_closure_1_pad_0(frame);
			frame->ok = false;
			return;
		}
		*captures->internal = STD_ADD(u64, *captures->internal, 1, 338, 17);
		if (trees_node_Node_level(&node) == 1)
		{
			*captures->leaves = STD_ADD(u64, *captures->leaves, (uint64) trees_node_Node_len(&node),
										340, 21);
			continue;
		}
		n = trees_node_Node_len(&node);
		hint = (StdSizeHint)
		{
			n, true, n
		};
		std_vec_extend_trusted_reserve(pending, hint, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
		for (size_t i = 0; i < n; i = std_forward_unchecked_usize_Checked(i, 1))
		{
			STD_VEC_AT(pending, TreesStoreTree, pending->len) = trees_node_Node_child_tree(&node, i);
			pending->len = std_wrapping_add_usize(pending->len, 1);
		}
	}
	trees_node_counts_closure_1_pad_0(frame);
	frame->ok = true;
}

/* trees_node_counts_closure_1 is node_counts' second pin closure: a batch of the pending trees counted. */
static bool
trees_node_counts_closure_1(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	trees_node_counts_closure_1_frame_0_t frame = {.captures = captures, .io = io, .pin = pin, .error = error};

	std_landing(&frame, trees_node_counts_closure_1_body_0, trees_node_counts_closure_1_pad_0);
	return frame.ok;
}

/* trees_node_counts_frame_0_t is node_counts' state across its pad: the pending trees, while live. */
typedef struct trees_node_counts_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreNamed tree;
	std_TupleU64U64_t *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		pending;
	bool		pending_live;
} trees_node_counts_frame_0_t;

/* trees_node_counts_pad_0 is node_counts' pad: the pending trees, while live. */
static void
trees_node_counts_pad_0(void *frame_)
{
	trees_node_counts_frame_0_t *frame = frame_;

	if (frame->pending_live)
		std_vec_free_Checked(&frame->pending, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
}

/* trees_node_counts_body_0 is node_counts' body: the tree's nodes walked a batch at a time from the root. */
static void
trees_node_counts_body_0(void *frame_)
{
	trees_node_counts_frame_0_t *frame = frame_;
	TreesStoreTree tree = trees_store_name(frame->tree);
	bool		leaf;
	uint64		internal = 0;
	uint64		leaves = 0;

	frame->ok = false;
	if (!trees_pin_pin_ResultBool(frame->io, frame->store.ops->as_dyn(frame->store.self), trees_node_counts_closure_0,
								  &tree, NULL, &leaf, frame->error))
		return;
	if (leaf)
	{
		frame->result->f0 = 0;
		frame->result->f1 = 1;
		frame->ok = true;
		return;
	}
	{
		TreesStoreTree *boxed = std_box_new_uninit_layout_Checked(sizeof(TreesStoreTree), _Alignof(TreesStoreTree));

		STD_CHECK_ALIGNED(boxed, _Alignof(TreesStoreTree), 333, 23);
		STD_CHECK_NULL_DEREF(boxed, 333, 23);
		boxed[0] = tree;
		std_box_slice_into_vec_Checked(&frame->pending, boxed, 1, sizeof(TreesStoreTree));
		frame->pending_live = true;
	}
	while (frame->pending.len != 0)
	{
		trees_node_counts_closure_1_captures_t captures = {&frame->pending, &internal, &leaves};

		if (!trees_pin_pin_ResultUnit(frame->io, frame->store.ops->as_dyn(frame->store.self),
									  trees_node_counts_closure_1, &captures, NULL, frame->error))
		{
			trees_node_counts_pad_0(frame);
			return;
		}
	}
	frame->result->f0 = internal;
	frame->result->f1 = leaves;
	trees_node_counts_pad_0(frame);
	frame->ok = true;
}

/* trees_node_counts returns the number of nodes of the tree above its leaves and the number of its leaves. */
bool
trees_node_counts(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, std_TupleU64U64_t *result,
				  ChunksErrorError *error)
{
	trees_node_counts_frame_0_t frame = {.io = io, .store = store, .tree = tree, .result = result, .error = error};

	std_landing(&frame, trees_node_counts_body_0, trees_node_counts_pad_0);
	return frame.ok;
}

/* trees_SpreadKeys_Default_default is SpreadKeys' derived Default: every list empty and every count 0. */
TreesSpreadKeys
trees_SpreadKeys_Default_default(void)
{
	TreesSpreadKeys result = {0};

	std_vec_new(&result.lasts, sizeof(trees_TupleVecU8Bool_t), _Alignof(trees_TupleVecU8Bool_t));
	std_vec_new(&result.leaves, sizeof(StdVec), _Alignof(StdVec));
	std_vec_new(&result.leaf_lasts, sizeof(size_t), _Alignof(size_t));
	return result;
}

/* trees_VecTupleVecU8Bool_drop is Vec<(Vec<u8>, bool)>'s drop glue: each key's, then the buffer. */
void
trees_VecTupleVecU8Bool_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(self, trees_TupleVecU8Bool_t, k).f0, 1, 1);
	std_vec_free_Checked(self, sizeof(trees_TupleVecU8Bool_t), _Alignof(trees_TupleVecU8Bool_t));
}

/* trees_VecVecU8_drop is Vec<Vec<u8>>'s drop glue: each vector's, then the buffer. */
void
trees_VecVecU8_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(self, StdVec, k), 1, 1);
	std_vec_free_Checked(self, sizeof(StdVec), _Alignof(StdVec));
}

/* trees_VecVecVecU8_drop is Vec<Vec<Vec<u8>>>'s drop glue: each list's, then the buffer. */
void
trees_VecVecVecU8_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		trees_VecVecU8_drop(&STD_VEC_AT(self, StdVec, k));
	std_vec_free_Checked(self, sizeof(StdVec), _Alignof(StdVec));
}

/* trees_SpreadKeys_drop is SpreadKeys' drop glue: its lasts, its leaves' keys, and its leaves' places. */
void
trees_SpreadKeys_drop(TreesSpreadKeys *self)
{
	trees_VecTupleVecU8Bool_drop(&self->lasts);
	trees_VecVecVecU8_drop(&self->leaves);
	std_vec_free_Checked(&self->leaf_lasts, sizeof(size_t), _Alignof(size_t));
}

/*
 * trees_spread_keys_closure_0 is spread_keys' evenly: all's indexes up to
 * the limit, or limit indexes spread evenly over them, its map's closure
 * inlined.
 */
static void
trees_spread_keys_closure_0(size_t all, size_t limit, StdVec *result)
{
	size_t		n = all <= limit ? all : limit;
	StdSizeHint hint = {n, true, n};

	std_vec_with_capacity_trusted(result, hint, sizeof(size_t), _Alignof(size_t));
	std_vec_extend_trusted_reserve(result, hint, sizeof(size_t), _Alignof(size_t));
	for (size_t i = 0; i < n; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		STD_VEC_AT(result, size_t, result->len) =
			all <= limit ? i : STD_DIV(usize, STD_MUL(usize, i, all, 380, 37), limit, 380, 37);
		result->len = std_wrapping_add_usize(result->len, 1);
	}
}

/* trees_spread_keys_closure_1_captures_t is spread_keys' pin closure's captures: the batch, the root, and the lists it fills. */
typedef struct trees_spread_keys_closure_1_captures_t
{
	const TreesStoreTree *batch;
	size_t		batch_len;
	const TreesStoreTree *tree;
	TreesSpreadKeys *spread;
	StdVec	   *leaf_trees;
	StdVec	   *children;
} trees_spread_keys_closure_1_captures_t;

/* trees_spread_keys_closure_1_frame_0_t is the pin closure's state across its pad: a leaf's keys and a last key, while live. */
typedef struct trees_spread_keys_closure_1_frame_0_t
{
	trees_spread_keys_closure_1_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	ChunksErrorError *error;
	bool		ok;
	StdVec		keys;
	bool		keys_live;
	trees_TupleVecU8Bool_t last;
	bool		last_live;
} trees_spread_keys_closure_1_frame_0_t;

/* trees_spread_keys_closure_1_pad_0 is the pin closure's pad: a leaf's keys and a last key, while live. */
static void
trees_spread_keys_closure_1_pad_0(void *frame_)
{
	trees_spread_keys_closure_1_frame_0_t *frame = frame_;

	if (frame->keys_live)
		trees_VecVecU8_drop(&frame->keys);
	if (frame->last_live)
		std_vec_free_Checked(&frame->last.f0, 1, 1);
}

/*
 * trees_spread_keys_closure_1_body_0 is the pin closure's body: each node of
 * the batch read, a leaf's keys kept, a level-1 node's leaves' last keys and
 * trees kept, and a higher node's children kept.
 */
static void
trees_spread_keys_closure_1_body_0(void *frame_)
{
	trees_spread_keys_closure_1_frame_0_t *frame = frame_;
	trees_spread_keys_closure_1_captures_t *captures = frame->captures;
	TreesSpreadKeys *spread = captures->spread;

	for (size_t k = 0, k_len = captures->batch_len; k < k_len; k++)
	{
		TreesStoreTree at = captures->batch[k];
		TreesNodeNode node;
		size_t		n;
		StdSizeHint hint;

		if (!trees_pin_Pin_node(frame->pin, frame->io, TREES_STORE_TREE_NAMED(&at), &node, frame->error))
		{
			frame->ok = false;
			return;
		}
		if (trees_store_Tree_PartialEq_eq(&at, captures->tree))
			spread->entries = trees_node_Node_total(&node);
		n = trees_node_Node_len(&node);
		switch (trees_node_Node_level(&node))
		{
			case 0:
				hint = (StdSizeHint)
				{
					n, true, n
				};
				std_vec_with_capacity_trusted(&frame->keys, hint, sizeof(StdVec), _Alignof(StdVec));
				frame->keys_live = true;
				std_vec_extend_trusted_reserve(&frame->keys, hint, sizeof(StdVec), _Alignof(StdVec));
				for (size_t i = 0; i < n; i = std_forward_unchecked_usize_Checked(i, 1))
				{
					StdBytes	key = trees_node_Node_key(&node, i);

					std_slice_to_vec_Checked(&STD_VEC_AT(&frame->keys, StdVec, frame->keys.len), key.ptr, key.len, 1,
											 1);
					frame->keys.len = std_wrapping_add_usize(frame->keys.len, 1);
				}
				std_vec_push_Checked(&spread->leaves, &frame->keys, sizeof(StdVec), _Alignof(StdVec));
				frame->keys_live = false;
				break;
			case 1:
				for (size_t i = 0; i < n; i = std_forward_unchecked_usize_Checked(i, 1))
				{
					StdBytes	key = trees_node_Node_key(&node, i);
					TreesStoreTree child;

					std_slice_to_vec_Checked(&frame->last.f0, key.ptr, key.len, 1, 1);
					frame->last.f1 = i > 0;
					frame->last_live = true;
					std_vec_push_Checked(&spread->lasts, &frame->last, sizeof(trees_TupleVecU8Bool_t),
										 _Alignof(trees_TupleVecU8Bool_t));
					frame->last_live = false;
					spread->lasts_entries = STD_ADD(u64, spread->lasts_entries, trees_node_Node_subtree(&node, i),
													399, 33);
					child = trees_node_Node_child_tree(&node, i);
					std_vec_push_Checked(captures->leaf_trees, &child, sizeof(TreesStoreTree),
										 _Alignof(TreesStoreTree));
				}
				break;
			default:
				hint = (StdSizeHint)
				{
					n, true, n
				};
				std_vec_extend_trusted_reserve(captures->children, hint, sizeof(TreesStoreTree),
											   _Alignof(TreesStoreTree));
				for (size_t i = 0; i < n; i = std_forward_unchecked_usize_Checked(i, 1))
				{
					STD_VEC_AT(captures->children, TreesStoreTree, captures->children->len) =
						trees_node_Node_child_tree(&node, i);
					captures->children->len = std_wrapping_add_usize(captures->children->len, 1);
				}
				break;
		}
	}
	frame->ok = true;
}

/* trees_spread_keys_closure_1 is spread_keys' pin closure: one batch of a level's nodes read. */
static bool
trees_spread_keys_closure_1(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	trees_spread_keys_closure_1_frame_0_t frame = {.captures = captures, .io = io, .pin = pin, .error = error};

	std_landing(&frame, trees_spread_keys_closure_1_body_0, trees_spread_keys_closure_1_pad_0);
	return frame.ok;
}

/*
 * trees_spread_keys_frame_0_t is spread_keys' state across its pad: the keys
 * spread so far, the level walked, the leaves' trees, a level's children,
 * the leaves taken, the picked indexes, and the next level, while live.
 */
typedef struct trees_spread_keys_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreNamed tree;
	size_t		nodes;
	size_t		leaves;
	TreesSpreadKeys *result;
	ChunksErrorError *error;
	bool		ok;
	TreesSpreadKeys spread;
	bool		spread_live;
	StdVec		level;
	bool		level_live;
	StdVec		leaf_trees;
	bool		leaf_trees_live;
	StdVec		children;
	bool		children_live;
	StdVec		all;
	bool		all_live;
	StdVec		picked;
	bool		picked_live;
	StdVec		next;
	bool		next_live;
} trees_spread_keys_frame_0_t;

/* trees_spread_keys_pad_0 is spread_keys' pad: what its frame holds, in reverse order. */
static void
trees_spread_keys_pad_0(void *frame_)
{
	trees_spread_keys_frame_0_t *frame = frame_;

	if (frame->next_live)
		std_vec_free_Checked(&frame->next, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	if (frame->picked_live)
		std_vec_free_Checked(&frame->picked, sizeof(size_t), _Alignof(size_t));
	if (frame->all_live)
		std_vec_free_Checked(&frame->all, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	if (frame->children_live)
		std_vec_free_Checked(&frame->children, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	if (frame->leaf_trees_live)
		std_vec_free_Checked(&frame->leaf_trees, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	if (frame->level_live)
		std_vec_free_Checked(&frame->level, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	if (frame->spread_live)
		trees_SpreadKeys_drop(&frame->spread);
}

/*
 * trees_spread_keys_body_0 is spread_keys' body: each level read in batches,
 * the next level picked evenly from its children, and at the leaves' level,
 * the leaves picked evenly to read every key of.
 */
static void
trees_spread_keys_body_0(void *frame_)
{
	trees_spread_keys_frame_0_t *frame = frame_;
	TreesStoreTree tree = trees_store_name(frame->tree);

	frame->spread = trees_SpreadKeys_Default_default();
	frame->spread_live = true;
	{
		TreesStoreTree *boxed = std_box_new_uninit_layout_Checked(sizeof(TreesStoreTree), _Alignof(TreesStoreTree));

		STD_CHECK_ALIGNED(boxed, _Alignof(TreesStoreTree), 383, 21);
		STD_CHECK_NULL_DEREF(boxed, 383, 21);
		boxed[0] = tree;
		std_box_slice_into_vec_Checked(&frame->level, boxed, 1, sizeof(TreesStoreTree));
		frame->level_live = true;
	}
	std_vec_new(&frame->leaf_trees, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	frame->leaf_trees_live = true;
	while (frame->level.len != 0)
	{
		StdChunks	chunks;
		const void *batch;
		size_t		batch_len;
		StdSizeHint hint;

		std_vec_new(&frame->children, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
		frame->children_live = true;
		std_slice_chunks_check(TREES_NODE_BATCH, RUST_FILE, 387, 28);
		chunks = std_slice_chunks(frame->level.ptr, frame->level.len, TREES_NODE_BATCH);
		while (std_slice_Chunks_next(&chunks, sizeof(TreesStoreTree), _Alignof(TreesStoreTree), &batch, &batch_len))
		{
			trees_spread_keys_closure_1_captures_t captures = {batch, batch_len, &tree, &frame->spread,
			&frame->leaf_trees, &frame->children};

			if (!trees_pin_pin_ResultUnit(frame->io, frame->store.ops->as_dyn(frame->store.self),
										  trees_spread_keys_closure_1, &captures, NULL, frame->error))
			{
				trees_spread_keys_pad_0(frame);
				frame->ok = false;
				return;
			}
		}
		if (frame->children.len == 0)
		{
			frame->all = frame->leaf_trees;
			frame->all_live = true;
			std_vec_new(&frame->leaf_trees, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
			trees_spread_keys_closure_0(frame->all.len, frame->leaves, &frame->picked);
			frame->picked_live = true;
			hint = (StdSizeHint)
			{
				frame->picked.len, true, frame->picked.len
			};
			std_vec_with_capacity_trusted(&frame->next, hint, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
			frame->next_live = true;
			std_vec_extend_trusted_reserve(&frame->next, hint, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
			for (size_t k = 0, k_len = frame->picked.len; k < k_len; k++)
			{
				size_t		i = STD_VEC_AT(&frame->picked, size_t, k);

				std_index_check(i, frame->all.len, RUST_FILE, 413, 55);
				STD_VEC_AT(&frame->next, TreesStoreTree, frame->next.len) = STD_VEC_AT(&frame->all, TreesStoreTree, i);
				frame->next.len = std_wrapping_add_usize(frame->next.len, 1);
			}
			std_vec_extend_from_slice_Checked(&frame->spread.leaf_lasts, frame->picked.ptr, frame->picked.len,
											  sizeof(size_t), _Alignof(size_t));
			frame->picked_live = false;
			std_vec_free_Checked(&frame->picked, sizeof(size_t), _Alignof(size_t));
			frame->all_live = false;
			std_vec_free_Checked(&frame->all, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
		}
		else
		{
			trees_spread_keys_closure_0(frame->children.len, frame->nodes, &frame->picked);
			frame->picked_live = true;
			hint = (StdSizeHint)
			{
				frame->picked.len, true, frame->picked.len
			};
			std_vec_with_capacity_trusted(&frame->next, hint, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
			frame->next_live = true;
			std_vec_extend_trusted_reserve(&frame->next, hint, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
			for (size_t k = 0, k_len = frame->picked.len; k < k_len; k++)
			{
				size_t		i = STD_VEC_AT(&frame->picked, size_t, k);

				std_index_check(i, frame->children.len, RUST_FILE, 417, 80);
				STD_VEC_AT(&frame->next, TreesStoreTree, frame->next.len) =
					STD_VEC_AT(&frame->children, TreesStoreTree, i);
				frame->next.len = std_wrapping_add_usize(frame->next.len, 1);
			}
			frame->picked_live = false;
			std_vec_free_Checked(&frame->picked, sizeof(size_t), _Alignof(size_t));
		}
		std_vec_free_Checked(&frame->level, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
		frame->level = frame->next;
		frame->next_live = false;
		frame->children_live = false;
		std_vec_free_Checked(&frame->children, sizeof(TreesStoreTree), _Alignof(TreesStoreTree));
	}
	frame->spread_live = false;
	*frame->result = frame->spread;
	trees_spread_keys_pad_0(frame);
	frame->ok = true;
}

/*
 * trees_spread_keys returns the keys of a tree spread evenly over it: the last
 * key of each leaf under at most nodes nodes of the level above the leaves,
 * picked evenly at every level, and every key of at most leaves of those
 * leaves picked evenly, or of the tree's one leaf.
 */
bool
trees_spread_keys(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, size_t nodes, size_t leaves,
				  TreesSpreadKeys *result, ChunksErrorError *error)
{
	trees_spread_keys_frame_0_t frame = {.io = io, .store = store, .tree = tree, .nodes = nodes, .leaves = leaves,
	.result = result, .error = error};

	std_landing(&frame, trees_spread_keys_body_0, trees_spread_keys_pad_0);
	return frame.ok;
}

/* trees_height_closure_0 is height's pin closure: the root's level. */
static bool
trees_height_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, uint8 *result, ChunksErrorError *error)
{
	TreesNodeNode node;

	if (!trees_pin_Pin_node(pin, io, TREES_STORE_TREE_NAMED(captures), &node, error))
		return false;
	*result = trees_node_Node_level(&node);
	return true;
}

/* trees_height returns the number of levels of the tree above its leaves. */
bool
trees_height(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint8 *result, ChunksErrorError *error)
{
	TreesStoreTree named = trees_store_name(tree);

	return trees_pin_pin_ResultU8(io, store.ops->as_dyn(store.self), trees_height_closure_0, &named, NULL, result,
								  error);
}

/* trees_empty returns the empty tree. */
bool
trees_empty(HostIoIo *io, TreesStoreBranded store, TreesStoreFresh *result, ChunksErrorError *error)
{
	TreesBuildBuilder builder;
	TreesStoreNodeStore node_store = {&store.ops->node_store, store.self};

	trees_build_Builder_new(node_store, TREES_CHUNKER_DEFAULT_TARGET, &builder);
	return trees_build_Builder_finish(&builder, io, store.ops, result, error);
}
