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
 * The C forms of vec::IntoIter (alloc/src/vec/into_iter.rs at Rust 1.99.1)
 * and iter::Map (core/src/iter/adapters/map.rs): an owning walk of a
 * vector, whose drop drops the elements left and frees the buffer even
 * should a drop panic, and a walk of another iterator's items through a
 * closure.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/any.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"

/* INDEX_RANGE_FILE is core's ops/index_range.rs, IndexRange's, which array::IntoIter's alive range is. */
#define INDEX_RANGE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "ops" STD_SEP "index_range.rs"

/* ITER_INNER_FILE is core's array/iter/iter_inner.rs, where array::IntoIter reads its elements. */
#define ITER_INNER_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "array" STD_SEP "iter" STD_SEP "iter_inner.rs"

/* EXACT_SIZE_FILE is core's iter/traits/exact_size.rs, where ExactSizeIterator::len asserts its size_hint. */
#define EXACT_SIZE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "iter" STD_SEP "traits" STD_SEP "exact_size.rs"

/*
 * std_IndexRange_next_unchecked is IndexRange's next_unchecked: the range's
 * first index, which it gives up, unchecked_add's check on the step.
 */
static size_t
std_IndexRange_next_unchecked(size_t *start)
{
	size_t		value = *start;

	std_usize_unchecked_add_check(value, 1, INDEX_RANGE_FILE, 63, 37);
	*start = value + 1;
	return value;
}

/*
 * std_array_IntoIter_next is array::IntoIter's next over an array of len
 * elements alive from *start to end: false when none is, else the index of
 * the element it moves out, which get_unchecked checks.
 */
bool
std_array_IntoIter_next(size_t *start, size_t end, size_t len, size_t *index)
{
	if (end - *start == 0)
		return false;
	*index = std_IndexRange_next_unchecked(start);
	std_slice_get_unchecked_check(*index, len, ITER_INNER_FILE, 219, 32);
	return true;
}

/* std_ArrayIntoIter_Iterator_next moves the next element alive to result, false when none is. */
bool
std_ArrayIntoIter_Iterator_next(void *self_, void *result)
{
	StdArrayIntoIter *self = self_;
	size_t		index;

	if (!std_array_IntoIter_next(&self->start, self->end, self->len, &index))
		return false;
	memcpy(result, (char *) self->data + index * self->size, self->size);
	return true;
}

/* std_ArrayIntoIter_Iterator_size_hint is array::IntoIter's size_hint: exactly the elements alive. */
StdSizeHint
std_ArrayIntoIter_Iterator_size_hint(const void *self_)
{
	const StdArrayIntoIter *self = self_;
	StdSizeHint hint;

	hint.lower = self->end - self->start;
	hint.has_upper = true;
	hint.upper = hint.lower;
	return hint;
}

/*
 * STD_ARRAY_INTO_ITER_ITERATOR_OPS is array::IntoIter's Iterator, of
 * elements whose drop does nothing.
 */
const StdIteratorOps STD_ARRAY_INTO_ITER_ITERATOR_OPS = {
	std_ArrayIntoIter_Iterator_next,
	std_ArrayIntoIter_Iterator_size_hint,
	NULL,
};

/* std_array_IntoIter_fold_begin is array::IntoIter's fold's start: IndexRange's assumption that it is ordered. */
void
std_array_IntoIter_fold_begin(size_t start, size_t end)
{
	std_hint_assert_unchecked(start <= end, INDEX_RANGE_FILE, 122, 18);
}

/*
 * std_array_IntoIter_fold_next is a step of array::IntoIter's fold over an
 * array of len elements alive from *start to end: false at the end, else
 * the index of the element it moves out, which get_unchecked checks.
 */
bool
std_array_IntoIter_fold_next(size_t *start, size_t end, size_t len, size_t *index)
{
	if (*start == end)
		return false;
	*index = std_IndexRange_next_unchecked(start);
	std_slice_get_unchecked_check(*index, len, ITER_INNER_FILE, 264, 38);
	return true;
}

/*
 * std_array_Drain_drop is array::Drain's drop over elements of a size with no
 * drop glue: the slice of those it hasn't yielded, its length by
 * offset_from_unsigned from the next to the end.
 */
void
std_array_Drain_drop(const void *next, const void *end, size_t size)
{
	(void) std_ptr_offset_from_unsigned_Checked(end, next, size, STD_ARRAY_DRAIN_FILE, 123, 37);
}

/*
 * std_array_Guard_drop is array's Guard drop over elements with no drop
 * glue: the initialized prefix of an array of len elements, which
 * get_unchecked_mut checks.
 */
void
std_array_Guard_drop(size_t initialized, size_t len)
{
	std_slice_get_unchecked_mut_range_check(0, initialized, len, STD_SLICE_INDEX_FILE, 498, 32);
}

/* std_Iterator_size_hint is Iterator's size_hint: no bound but zero, for an iterator that gives none. */
StdSizeHint
std_Iterator_size_hint(const void *self)
{
	StdSizeHint hint = {0, false, 0};

	return hint;
}

/*
 * std_FlattenCompat_size_hint is a FlatMap's size_hint over items of a fixed
 * size before its first item, when its front and back iterators are None.
 */
StdSizeHint
std_FlattenCompat_size_hint(StdSizeHint iter, size_t fixed_size)
{
	size_t		flo = 0;
	size_t		fhi = 0;
	size_t		blo = 0;
	size_t		bhi = 0;
	size_t		lo = std_saturating_add_usize(flo, blo);
	size_t		front_back;
	size_t		inner;
	StdSizeHint hint = {0, false, 0};

	hint.lower = std_saturating_add_usize(std_saturating_mul_usize(iter.lower, fixed_size), lo);
	hint.has_upper = std_checked_add_usize(fhi, bhi, &front_back) && iter.has_upper &&
		std_checked_mul_usize(fixed_size, iter.upper, &inner) &&
		std_checked_add_usize(front_back, inner, &hint.upper);
	return hint;
}

/* std_Vec_IntoIterator_into_iter is Vec's into_iter: a walk that takes the vector. */
StdVecIntoIter
std_Vec_IntoIterator_into_iter(StdVec *v, size_t size, size_t align, void (*drop) (void *elem))
{
	StdVecIntoIter iter;

	iter.buf = *v;
	iter.next = 0;
	iter.size = size;
	iter.align = align;
	iter.drop = drop;
	return iter;
}

/* std_VecIntoIter_Iterator_next moves the next element to result, false when none is left. */
bool
std_VecIntoIter_Iterator_next(void *self_, void *result)
{
	StdVecIntoIter *self = self_;

	if (self->next == self->buf.len)
		return false;
	memcpy(result, (char *) self->buf.ptr + self->next * self->size, self->size);
	self->next++;
	return true;
}

/*
 * std_VecIntoIter_Iterator_size_hint is the exact count of elements left,
 * NonNull::offset_from_unsigned's of the end and the next element but for a
 * zero-sized type's.
 */
StdSizeHint
std_VecIntoIter_Iterator_size_hint(const void *self_)
{
	const StdVecIntoIter *self = self_;
	size_t		exact = self->buf.len - self->next;
	StdSizeHint hint;

	if (self->size != 0)
		exact = std_ptr_offset_from_unsigned_Checked((const char *) self->buf.ptr + self->buf.len * self->size,
													 (const char *) self->buf.ptr + self->next * self->size,
													 self->size, STD_PTR_NON_NULL_FILE, 898, 32);
	hint.lower = exact;
	hint.has_upper = true;
	hint.upper = exact;
	return hint;
}

/* std_VecIntoIter_len is ExactSizeIterator::len: size_hint's lower bound, asserted equal to its upper. */
size_t
std_VecIntoIter_len(const StdVecIntoIter *self)
{
	StdSizeHint hint = std_VecIntoIter_Iterator_size_hint(self);
	StdOptionRef upper = {hint.has_upper, {&hint.upper, std_usize_Debug_fmt}};
	StdOptionRef some_lower = {true, {&hint.lower, std_usize_Debug_fmt}};

	if (!(upper.has == some_lower.has && hint.upper == hint.lower))
		std_assert_failed(EXACT_SIZE_FILE, 122, 9, "==", &upper, std_option_Debug_fmt, &some_lower,
						  std_option_Debug_fmt, NULL);
	return hint.lower;
}

/*
 * std_vec_extend_trusted is Vec's extend_trusted: room reserved for a
 * TrustedLen iterator's upper bound, then each item written past the length.
 */
static void
std_vec_extend_trusted(StdVec *v, StdIterator *iter, size_t size, size_t align)
{
	StdSizeHint hint = iter->ops->size_hint(iter->self);

	std_vec_extend_trusted_reserve(v, hint, size, align);
	while (iter->ops->next(iter->self, (char *) v->ptr + v->len * size))
		v->len++;
}

/*
 * std_vec_splice is Vec::splice of a TrustedLen iterator: the range's drain,
 * and the iterator, which it takes and drops should the drain panic.
 */
StdVecSplice
std_vec_splice(StdVec *v, StdBound start, StdBound end, size_t size, size_t align, void (*drop) (void *),
			   StdIterator replace_with)
{
	StdVecSplice splice;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		splice.drain = std_vec_drain(v, start, end, size, align, drop);
		std_unwind_pop(&unwind);
	}
	else
	{
		std_iterator_drop(&replace_with);
		std_unwind_resume(&unwind);
	}
	splice.replace_with = replace_with;
	return splice;
}

/*
 * std_VecDrain_fill is Drain's fill: the drained range from the vector's
 * length to the tail filled from an iterator, false when it ends first.
 */
static bool
std_VecDrain_fill(StdVecDrain *self, StdIterator *replace_with)
{
	StdVec	   *vec = self->vec;

	for (size_t idx = vec->len, idx_end = self->tail_start; idx < idx_end; idx++)
	{
		if (!replace_with->ops->next(replace_with->self, (char *) vec->ptr + idx * self->size))
			return false;
		vec->len++;
	}
	return true;
}

/* std_VecDrain_move_tail is Drain's move_tail: room for more past the tail, and the tail moved that much later. */
static void
std_VecDrain_move_tail(StdVecDrain *self, size_t additional)
{
	StdVec	   *vec = self->vec;
	size_t		len = self->tail_start + self->tail_len;
	size_t		new_tail_start;

	std_RawVec_reserve_Checked(vec, len, additional, self->size, self->align);
	new_tail_start = self->tail_start + additional;
	std_ptr_copy((char *) vec->ptr + self->tail_start * self->size, (char *) vec->ptr + new_tail_start * self->size,
				 self->tail_len, self->size, self->align);
	self->tail_start = new_tail_start;
}

/*
 * std_VecSplice_drop is Splice's drop: the drained elements dropped, then
 * the range filled from the iterator, the tail moved later for its lower
 * bound and then for the rest it collects, and then its fields dropped, the
 * drain moving the tail back, should any step panic too.
 */
void
std_VecSplice_drop(StdVecSplice *self)
{
	volatile StdVec collecting;
	volatile bool collecting_live = false;
	volatile StdVecIntoIter collected;
	volatile bool collected_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVecDrain *drain = &self->drain;
		StdVec	   *collecting_v = (StdVec *) &collecting;
		StdVecIntoIter *collected_v = (StdVecIntoIter *) &collected;
		StdSizeHint hint;

		while (drain->next != drain->end)
		{
			void	   *elem = (char *) drain->vec->ptr + drain->next * drain->size;

			drain->next++;
			if (drain->drop != NULL)
				drain->drop(elem);
		}
		if (drain->tail_len == 0)
		{
			std_vec_extend_trusted(drain->vec, &self->replace_with, drain->size, drain->align);
			goto fields;
		}
		if (!std_VecDrain_fill(drain, &self->replace_with))
			goto fields;
		hint = self->replace_with.ops->size_hint(self->replace_with.self);
		if (hint.lower > 0)
		{
			std_VecDrain_move_tail(drain, hint.lower);
			if (!std_VecDrain_fill(drain, &self->replace_with))
				goto fields;
		}
		hint = self->replace_with.ops->size_hint(self->replace_with.self);
		std_vec_with_capacity_trusted(collecting_v, hint, drain->size, drain->align);
		collecting_live = true;
		std_vec_extend_trusted(collecting_v, &self->replace_with, drain->size, drain->align);
		collecting_live = false;
		*collected_v = std_Vec_IntoIterator_into_iter(collecting_v, drain->size, drain->align, drain->drop);
		collected_live = true;
		if (std_VecIntoIter_len(collected_v) > 0)
		{
			StdIterator rest = {&STD_VEC_INTO_ITER_ITERATOR_OPS, collected_v};

			std_VecDrain_move_tail(drain, std_VecIntoIter_len(collected_v));
			(void) std_VecDrain_fill(drain, &rest);
		}
		collected_live = false;
		std_VecIntoIter_drop(collected_v);
fields:
		std_VecDrain_drop(drain);
		std_iterator_drop(&self->replace_with);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (collected_live)
			std_VecIntoIter_drop((StdVecIntoIter *) &collected);
		if (collecting_live)
			std_vec_drop_Checked((StdVec *) &collecting, self->drain.size, self->drain.align, self->drain.drop);
		std_VecDrain_drop(&self->drain);
		std_iterator_drop(&self->replace_with);
		std_unwind_resume(&unwind);
	}
}

/*
 * std_VecIntoIter_drop drops the elements left, each in turn even should
 * one's drop panic, then frees the buffer, as IntoIter's drop and its
 * guard do; as_raw_mut_slice takes their count by ExactSizeIterator::len.
 */
void
std_VecIntoIter_drop(void *self_)
{
	StdVecIntoIter *self = self_;
	volatile size_t i = self->next;
	StdUnwind	unwind;

	(void) std_VecIntoIter_len(self);
	if (self->drop != NULL)
	{
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			for (; i < self->buf.len; i++)
				self->drop((char *) self->buf.ptr + i * self->size);
			std_unwind_pop(&unwind);
		}
		else
		{
			for (i++; i < self->buf.len; i++)
				self->drop((char *) self->buf.ptr + i * self->size);
			std_vec_free_Checked(&self->buf, self->size, self->align);
			std_unwind_resume(&unwind);
		}
	}
	std_vec_free_Checked(&self->buf, self->size, self->align);
}

/* STD_VEC_INTO_ITER_ITERATOR_OPS is IntoIter's table of Iterator. */
const StdIteratorOps STD_VEC_INTO_ITER_ITERATOR_OPS = {
	std_VecIntoIter_Iterator_next, std_VecIntoIter_Iterator_size_hint, std_VecIntoIter_drop
};

/*
 * std_vec_extend_one_unchecked_Checked is Vec's Extend::extend_one_unchecked
 * where a crate codegens it: the item written at the length into room
 * extend_reserve made, then set_len's checked length one longer, which wraps
 * as alloc's own arithmetic does.
 */
static void
std_vec_extend_one_unchecked_Checked(StdVec *v, const void *item, size_t size)
{
	size_t		len = v->len;

	memcpy(STD_PTR_AT(v->ptr, len, size), item, size);
	std_vec_set_len_Checked(v, std_wrapping_add_usize(len, 1));
}

/*
 * std_Tuple2_extend_reserve_Checked is (ExA, ExB)'s Extend::extend_reserve
 * at two Vecs, impl_extend_tuple!'s: Vec::reserve of each in order.
 */
static void
std_Tuple2_extend_reserve_Checked(StdVec *a, size_t a_size, size_t a_align, StdVec *b, size_t b_size,
								  size_t b_align, size_t additional)
{
	std_vec_reserve_Checked(a, additional, a_size, a_align);
	std_vec_reserve_Checked(b, additional, b_size, b_align);
}

/*
 * std_Tuple2_extend_one_unchecked_Checked is (ExA, ExB)'s
 * Extend::extend_one_unchecked at two Vecs: each part of the pair at its
 * offset into its Vec, in order.
 */
static void
std_Tuple2_extend_one_unchecked_Checked(StdVec *a, size_t a_offset, size_t a_size, StdVec *b, size_t b_offset,
										size_t b_size, const void *item)
{
	std_vec_extend_one_unchecked_Checked(a, STD_PTR_AT(item, a_offset, 1), a_size);
	std_vec_extend_one_unchecked_Checked(b, STD_PTR_AT(item, b_offset, 1), b_size);
}

/*
 * std_Iterator_unzip_VecIntoIter_frame_0_t is the unzip of a vec::IntoIter's
 * state across its pad: its source, its pair's parts' offsets, sizes, and
 * alignments, its unzipped pair of Vecs, and whether the source is still its
 * own.
 */
typedef struct std_Iterator_unzip_VecIntoIter_frame_0_t
{
	StdVecIntoIter *iter;
	size_t		a_offset;
	size_t		a_size;
	size_t		a_align;
	size_t		b_offset;
	size_t		b_size;
	size_t		b_align;
	StdVec	   *a;
	StdVec	   *b;
	bool		iter_live;
} std_Iterator_unzip_VecIntoIter_frame_0_t;

/*
 * std_Iterator_unzip_VecIntoIter_pad_0 is the unzip of a vec::IntoIter's
 * pad, should a reserve unwind: the IntoIter's drop while it is live, then
 * the unzipped pair's, a's and b's, which hold no element yet.
 */
static void
std_Iterator_unzip_VecIntoIter_pad_0(void *frame_)
{
	std_Iterator_unzip_VecIntoIter_frame_0_t *frame = frame_;

	if (frame->iter_live)
		std_VecIntoIter_drop(frame->iter);
	std_vec_free_Checked(frame->a, frame->a_size, frame->a_align);
	std_vec_free_Checked(frame->b, frame->b_size, frame->b_align);
}

/*
 * std_Iterator_unzip_VecIntoIter_body_0 is the unzip of a vec::IntoIter:
 * the empty pair, default_extend's TrustedLen SpecExtend's extend_reserve of
 * the exact count when it is above zero, and IntoIter's fold of each pair
 * into extend_one_unchecked, whose end frees the IntoIter's buffer, every
 * element having moved out.
 */
static void
std_Iterator_unzip_VecIntoIter_body_0(void *frame_)
{
	std_Iterator_unzip_VecIntoIter_frame_0_t *frame = frame_;
	StdVecIntoIter *iter = frame->iter;
	StdSizeHint hint = std_VecIntoIter_Iterator_size_hint(iter);

	if (hint.lower > 0)
		std_Tuple2_extend_reserve_Checked(frame->a, frame->a_size, frame->a_align, frame->b, frame->b_size,
										  frame->b_align, hint.lower);
	frame->iter_live = false;
	while (iter->next != iter->buf.len)
	{
		const void *tmp = STD_PTR_AT(iter->buf.ptr, iter->next, iter->size);

		iter->next = std_wrapping_add_usize(iter->next, 1);
		std_Tuple2_extend_one_unchecked_Checked(frame->a, frame->a_offset, frame->a_size, frame->b, frame->b_offset,
												frame->b_size, tmp);
	}
	std_vec_free_Checked(&iter->buf, iter->size, iter->align);
}

/*
 * std_Iterator_unzip_VecIntoIter_Checked is Iterator::unzip of a
 * vec::IntoIter of pairs, which it takes, into two Vecs, where a crate
 * codegens it. a and b are the pair's parts' offsets, sizes, and alignments;
 * should a reserve unwind, the pad drops the IntoIter and the pair.
 */
void
std_Iterator_unzip_VecIntoIter_Checked(StdVecIntoIter *iter, size_t a_offset, size_t a_size, size_t a_align,
									   size_t b_offset, size_t b_size, size_t b_align, StdVec *a, StdVec *b)
{
	std_Iterator_unzip_VecIntoIter_frame_0_t frame = {.iter = iter, .a_offset = a_offset, .a_size = a_size, .a_align = a_align,
	.b_offset = b_offset, .b_size = b_size, .b_align = b_align, .a = a, .b = b, .iter_live = true};

	std_vec_new(a, a_size, a_align);
	std_vec_new(b, b_size, b_align);
	std_landing(&frame, std_Iterator_unzip_VecIntoIter_body_0, std_Iterator_unzip_VecIntoIter_pad_0);
}

/*
 * std_Iterator_unzip_CopiedSliceIter_frame_0_t is the unzip of a Copied
 * slice::Iter's state across its pad: its slice, its pair's parts' offsets,
 * sizes, and alignments, and its unzipped pair of Vecs.
 */
typedef struct std_Iterator_unzip_CopiedSliceIter_frame_0_t
{
	const void *v;
	size_t		len;
	size_t		size;
	size_t		a_offset;
	size_t		a_size;
	size_t		a_align;
	size_t		b_offset;
	size_t		b_size;
	size_t		b_align;
	StdVec	   *a;
	StdVec	   *b;
} std_Iterator_unzip_CopiedSliceIter_frame_0_t;

/*
 * std_Iterator_unzip_CopiedSliceIter_pad_0 is the unzip of a Copied
 * slice::Iter's pad, should a reserve unwind: the unzipped pair's drop, a's
 * and b's, which hold no element yet.
 */
static void
std_Iterator_unzip_CopiedSliceIter_pad_0(void *frame_)
{
	std_Iterator_unzip_CopiedSliceIter_frame_0_t *frame = frame_;

	std_vec_free_Checked(frame->a, frame->a_size, frame->a_align);
	std_vec_free_Checked(frame->b, frame->b_size, frame->b_align);
}

/*
 * std_Iterator_unzip_CopiedSliceIter_body_0 is the unzip of a Copied
 * slice::Iter: the empty pair, default_extend's TrustedLen SpecExtend's
 * extend_reserve of the slice's length when it is above zero, and Copied's
 * fold through slice::Iter's, each pair copied into extend_one_unchecked,
 * its index stepped by unchecked_add. size_hint's and fold's len! are
 * offset_from_unsigned's, whose preconditions, with unchecked_add's, the
 * iterator! invocation's site reports.
 */
static void
std_Iterator_unzip_CopiedSliceIter_body_0(void *frame_)
{
	std_Iterator_unzip_CopiedSliceIter_frame_0_t *frame = frame_;
	const void *end = STD_PTR_AT(frame->v, frame->len, frame->size);
	size_t		lower = std_ptr_offset_from_unsigned_Checked(end, frame->v, frame->size, STD_SLICE_ITER_FILE, 140, 1);
	size_t		len;

	if (lower > 0)
		std_Tuple2_extend_reserve_Checked(frame->a, frame->a_size, frame->a_align, frame->b, frame->b_size,
										  frame->b_align, lower);
	if (frame->v == end)
		return;
	len = std_ptr_offset_from_unsigned_Checked(end, frame->v, frame->size, STD_SLICE_ITER_FILE, 140, 1);
	for (size_t i = 0;;)
	{
		std_Tuple2_extend_one_unchecked_Checked(frame->a, frame->a_offset, frame->a_size, frame->b, frame->b_offset,
												frame->b_size, STD_PTR_AT(frame->v, i, frame->size));
		std_usize_unchecked_add_check(i, 1, STD_SLICE_ITER_FILE, 140, 1);
		i = std_wrapping_add_usize(i, 1);
		if (i == len)
			break;
	}
}

/*
 * std_Iterator_unzip_CopiedSliceIter_Checked is Iterator::unzip of a
 * Copied<slice::Iter> of pairs into two Vecs, where a crate codegens it. a
 * and b are the pair's parts' offsets, sizes, and alignments; should a
 * reserve unwind, the pad drops the pair.
 */
void
std_Iterator_unzip_CopiedSliceIter_Checked(const void *v, size_t len, size_t size, size_t a_offset, size_t a_size,
										   size_t a_align, size_t b_offset, size_t b_size, size_t b_align,
										   StdVec *a, StdVec *b)
{
	std_Iterator_unzip_CopiedSliceIter_frame_0_t frame = {.v = v, .len = len, .size = size, .a_offset = a_offset, .a_size = a_size,
	.a_align = a_align, .b_offset = b_offset, .b_size = b_size, .b_align = b_align, .a = a, .b = b};

	std_vec_new(a, a_size, a_align);
	std_vec_new(b, b_size, b_align);
	std_landing(&frame, std_Iterator_unzip_CopiedSliceIter_body_0, std_Iterator_unzip_CopiedSliceIter_pad_0);
}
