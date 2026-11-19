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
 * The C port of alloc's VecDeque<T> (alloc/src/collections/vec_deque at
 * Rust 1.99.1) at a T of a size and alignment: new, is_empty, front,
 * pop_front, reserve with handle_capacity_increase, the TrustedLen extend's
 * writes at the back, the extend from a vec::IntoIter, and the drop. Rust's
 * std is built without debug assertions, so their debug_assert!s have no
 * form, and its arithmetic is plain C's.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec_deque.h"

/* std_vec_deque_wrap_index is wrap_index: a logical index past the capacity brought back into the buffer. */
static size_t
std_vec_deque_wrap_index(size_t logical_index, size_t capacity)
{
	if (logical_index >= capacity)
		return std_wrapping_sub_usize(logical_index, capacity);
	return logical_index;
}

/* std_vec_deque_to_wrapped_index is to_wrapped_index, wrap_add of the head: the physical index of a logical one. */
static size_t
std_vec_deque_to_wrapped_index(const StdVecDeque *self, size_t idx)
{
	return std_vec_deque_wrap_index(std_wrapping_add_usize(self->head, idx), self->cap);
}

/* std_vec_deque_new is VecDeque::new: no elements in RawVec's dangling block of no capacity. */
void
std_vec_deque_new(StdVecDeque *self, size_t size, size_t align)
{
	StdVec		buf;

	std_vec_new(&buf, size, align);
	self->ptr = buf.ptr;
	self->cap = buf.cap;
	self->head = 0;
	self->len = 0;
}

/* std_vec_deque_is_empty is VecDeque::is_empty: whether it has no elements. */
bool
std_vec_deque_is_empty(const StdVecDeque *self)
{
	return self->len == 0;
}

/* std_vec_deque_front_Checked is VecDeque::front, get(0): the first element, or NULL for none. */
const void *
std_vec_deque_front_Checked(const StdVecDeque *self, size_t size)
{
	if (0 < self->len)
		return STD_PTR_AT(self->ptr, std_vec_deque_to_wrapped_index(self, 0), size);
	return NULL;
}

/*
 * std_vec_deque_pop_front_Checked is VecDeque::pop_front: the first element
 * moved into result and true, or false for none, with assert_unchecked's
 * check that the length left is below the capacity.
 */
bool
std_vec_deque_pop_front_Checked(StdVecDeque *self, void *result, size_t size)
{
	size_t		old_head;

	if (std_vec_deque_is_empty(self))
		return false;
	old_head = self->head;
	self->head = std_vec_deque_to_wrapped_index(self, 1);
	self->len = std_wrapping_sub_usize(self->len, 1);
	std_hint_assert_unchecked(self->len < self->cap, STD_VEC_DEQUE_FILE, 2205, 17);
	memcpy(result, STD_PTR_AT(self->ptr, old_head, size), size);
	return true;
}

/*
 * std_vec_deque_handle_capacity_increase is handle_capacity_increase: after
 * the buffer grew from old_capacity, the elements that wrapped past its old
 * end moved so that they are contiguous again, the shorter run copied.
 */
static void
std_vec_deque_handle_capacity_increase(StdVecDeque *self, size_t old_capacity, size_t size, size_t align)
{
	size_t		new_capacity = self->cap;

	if (self->head <= std_wrapping_sub_usize(old_capacity, self->len))
		return;
	{
		size_t		head_len = std_wrapping_sub_usize(old_capacity, self->head);
		size_t		tail_len = std_wrapping_sub_usize(self->len, head_len);

		if (head_len > tail_len && std_wrapping_sub_usize(new_capacity, old_capacity) >= tail_len)
			std_ptr_copy_nonoverlapping_Checked(STD_PTR_AT(self->ptr, 0, size), STD_PTR_AT(self->ptr, old_capacity, size),
												tail_len, size, align);
		else
		{
			size_t		new_head = std_wrapping_sub_usize(new_capacity, head_len);

			std_ptr_copy(STD_PTR_AT(self->ptr, self->head, size), STD_PTR_AT(self->ptr, new_head, size), head_len,
						 size, align);
			self->head = new_head;
		}
	}
}

/*
 * std_vec_deque_reserve_Checked is VecDeque::reserve: room for at least
 * additional more elements, the sum's overflow a "capacity overflow" at its
 * expect, the buffer grown as RawVec::reserve grows it past the length.
 */
void
std_vec_deque_reserve_Checked(StdVecDeque *self, size_t additional, size_t size, size_t align)
{
	size_t		old_cap = self->cap;

	if (additional > std_wrapping_sub_usize(SIZE_MAX, self->len))
	{
		static const char text[] = "capacity overflow";

		std_option_expect_failed(text, sizeof(text) - 1, STD_VEC_DEQUE_FILE, 1108, 56);
	}
	if (std_wrapping_add_usize(self->len, additional) > old_cap)
	{
		StdVec		buf = {self->ptr, self->len, self->cap};

		std_vec_reserve_Checked(&buf, additional, size, align);
		self->ptr = buf.ptr;
		self->cap = buf.cap;
		std_vec_deque_handle_capacity_increase(self, old_cap, size, align);
	}
}

/*
 * std_vec_deque_back_slot is the slot a TrustedLen extend's
 * write_iter_wrapping writes its next element to, after its reserve of
 * them all: the physical index past the last element. Its caller writes the
 * element and adds it to the length, as the extend's guard counts what it
 * wrote.
 */
void *
std_vec_deque_back_slot(StdVecDeque *self, size_t size)
{
	return STD_PTR_AT(self->ptr, std_vec_deque_to_wrapped_index(self, self->len), size);
}

/* std_vec_deque_extend_vec_frame_0_t is the extend's state across its pad: the vector it takes, whose elements are its own until they are copied in. */
typedef struct std_vec_deque_extend_vec_frame_0_t
{
	StdVecDeque *self;
	StdVec	   *iterator;
	size_t		size;
	size_t		align;
	void		(*drop) (void *);
	bool		iterator_live;
} std_vec_deque_extend_vec_frame_0_t;

/* std_vec_deque_extend_vec_body_0 is the extend's body: room reserved, the slice copied in after the last element, wrapping past the buffer's end, the vector's block freed. */
static void
std_vec_deque_extend_vec_body_0(void *frame_)
{
	std_vec_deque_extend_vec_frame_0_t *frame = frame_;
	StdVecDeque *self = frame->self;
	const void *slice = frame->iterator->ptr;
	size_t		slice_len = frame->iterator->len;
	size_t		size = frame->size;
	size_t		align = frame->align;
	size_t		dst;
	size_t		head_room;

	std_vec_deque_reserve_Checked(self, slice_len, size, align);
	dst = std_vec_deque_to_wrapped_index(self, self->len);
	head_room = std_wrapping_sub_usize(self->cap, dst);
	if (slice_len <= head_room)
		std_ptr_copy_nonoverlapping_Checked(slice, STD_PTR_AT(self->ptr, dst, size), slice_len, size, align);
	else
	{
		const void *left;
		size_t		left_len;
		const void *right;
		size_t		right_len;

		std_slice_split_at_Checked(slice, slice_len, head_room, size, align, &left, &left_len, &right, &right_len,
								   STD_VEC_DEQUE_FILE, 557, 37);
		std_ptr_copy_nonoverlapping_Checked(left, STD_PTR_AT(self->ptr, dst, size), left_len, size, align);
		std_ptr_copy_nonoverlapping_Checked(right, self->ptr, right_len, size, align);
	}
	self->len = std_wrapping_add_usize(self->len, slice_len);
	frame->iterator_live = false;
	frame->iterator->len = 0;
	std_vec_free_Checked(frame->iterator, size, align);
}

/* std_vec_deque_extend_vec_pad_0 is the extend's pad: the vector's IntoIter's drop, its elements and block, while live. */
static void
std_vec_deque_extend_vec_pad_0(void *frame_)
{
	std_vec_deque_extend_vec_frame_0_t *frame = frame_;

	if (frame->iterator_live)
	{
		std_ptr_drop_in_place_slice(frame->iterator->ptr, frame->iterator->len, frame->size, frame->drop);
		frame->iterator->len = 0;
		std_vec_free_Checked(frame->iterator, frame->size, frame->align);
	}
}

/*
 * std_vec_deque_extend_vec_Checked is VecDeque's SpecExtend for a
 * vec::IntoIter: room reserved for the vector's elements, which copy_slice
 * moves in after the last, then the vector's block freed with nothing left
 * in it to drop; it takes the vector, whose elements drop with it should the
 * reserve unwind.
 */
void
std_vec_deque_extend_vec_Checked(StdVecDeque *self, StdVec *iterator, size_t size, size_t align,
								 void (*drop) (void *))
{
	std_vec_deque_extend_vec_frame_0_t frame = {.self = self, .iterator = iterator, .size = size, .align = align,
	.drop = drop, .iterator_live = true};

	std_landing(&frame, std_vec_deque_extend_vec_body_0, std_vec_deque_extend_vec_pad_0);
}

/* std_vec_deque_drop_frame_0_t is the drop's state across its pad: the deque, its two slices, and how far the drop got. */
typedef struct std_vec_deque_drop_frame_0_t
{
	StdVecDeque *self;
	size_t		size;
	size_t		align;
	void		(*drop) (void *);
	void	   *front;
	size_t		front_len;
	void	   *back;
	size_t		back_len;
	bool		back_live;
} std_vec_deque_drop_frame_0_t;

/* std_vec_deque_drop_body_0 is the drop's body: the front slice's elements dropped, then the back's, then the buffer freed. */
static void
std_vec_deque_drop_body_0(void *frame_)
{
	std_vec_deque_drop_frame_0_t *frame = frame_;
	StdVec		buf = {frame->self->ptr, 0, frame->self->cap};

	std_ptr_drop_in_place_slice(frame->front, frame->front_len, frame->size, frame->drop);
	frame->back_live = false;
	std_ptr_drop_in_place_slice(frame->back, frame->back_len, frame->size, frame->drop);
	std_vec_free_Checked(&buf, frame->size, frame->align);
}

/* std_vec_deque_drop_pad_0 is the drop's pad: the back slice, should the front's drop unwind, then the buffer. */
static void
std_vec_deque_drop_pad_0(void *frame_)
{
	std_vec_deque_drop_frame_0_t *frame = frame_;
	StdVec		buf = {frame->self->ptr, 0, frame->self->cap};

	if (frame->back_live)
		std_ptr_drop_in_place_slice(frame->back, frame->back_len, frame->size, frame->drop);
	std_vec_free_Checked(&buf, frame->size, frame->align);
}

/*
 * std_vec_deque_drop is VecDeque's Drop: as_mut_slices' front slice
 * dropped, its back slice dropped after by its Dropper should the front's
 * drop unwind, then RawVec's block freed.
 */
void
std_vec_deque_drop(StdVecDeque *self, size_t size, size_t align, void (*drop) (void *))
{
	std_vec_deque_drop_frame_0_t frame = {.self = self, .size = size, .align = align, .drop = drop,
	.back_live = true};

	if (self->len != 0)
	{
		size_t		wrapped_start = std_vec_deque_to_wrapped_index(self, 0);
		size_t		head_len = std_wrapping_sub_usize(self->cap, wrapped_start);

		frame.front = STD_PTR_AT(self->ptr, wrapped_start, size);
		if (head_len >= self->len)
			frame.front_len = self->len;
		else
		{
			frame.front_len = head_len;
			frame.back = self->ptr;
			frame.back_len = std_wrapping_sub_usize(self->len, head_len);
		}
	}
	std_landing(&frame, std_vec_deque_drop_body_0, std_vec_deque_drop_pad_0);
}
