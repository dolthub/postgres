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
 * alloc's Rc<T> (the port text's section 9) at a value's size and
 * alignment: new, Deref, Clone's strong count, Drop's strong and weak
 * counts with drop_slow's Weak freeing the block even should the value's
 * drop unwind, and Debug through the value's.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/rc.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* STD_RC_FILE is alloc's rc.rs, where inc_strong's assert_unchecked reports. */
#define STD_RC_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "rc.rs"

/* STD_RC_HEADER is RcInner<()>'s size: its strong and weak counts. */
#define STD_RC_HEADER (2 * sizeof(size_t))

/* std_rc_data_offset_alignment is data_offset_alignment: RcInner<()>'s size padded to the value's alignment. */
static size_t
std_rc_data_offset_alignment(size_t align)
{
	return std_wrapping_add_usize(STD_RC_HEADER, std_wrapping_sub_usize(align, 1)) & ~std_wrapping_sub_usize(align, 1);
}

/* std_rc_inner_align is RcInner<T>'s alignment: a usize's, or the value's when greater. */
static size_t
std_rc_inner_align(size_t align)
{
	return std_max_usize(_Alignof(size_t), align);
}

/* std_rc_inner_size is RcInner<T>'s size: the value at its data offset, padded to RcInner's alignment. */
static size_t
std_rc_inner_size(size_t size, size_t align)
{
	size_t		inner_align = std_rc_inner_align(align);

	size_t		end = std_wrapping_add_usize(std_rc_data_offset_alignment(align), size);

	return std_wrapping_add_usize(end, std_wrapping_sub_usize(inner_align, 1)) & ~std_wrapping_sub_usize(inner_align, 1);
}

/*
 * std_Rc_new is Rc::new: Box::new's block for an RcInner with a strong and a
 * weak count of one and the value moved in, an allocation that fails being
 * handle_alloc_error's.
 */
void *
std_Rc_new(const void *value, size_t size, size_t align)
{
	void	   *ptr = std_box_new_uninit_layout_Checked(std_rc_inner_size(size, align), std_rc_inner_align(align));

	((size_t *) ptr)[0] = 1;
	((size_t *) ptr)[1] = 1;
	memcpy(STD_PTR_AT(ptr, std_rc_data_offset_alignment(align), 1), value, size);
	return ptr;
}

/* std_Rc_deref is Rc's Deref: the value in its block. */
void *
std_Rc_deref(void *ptr, size_t align)
{
	return STD_PTR_AT(ptr, std_rc_data_offset_alignment(align), 1);
}

/*
 * std_Rc_clone is Rc's Clone where a crate codegens it: inc_strong's
 * assert_unchecked of a strong count above zero, checked, then the count one
 * higher, wrapping, an abort when it wraps to zero.
 */
void
std_Rc_clone(void *ptr)
{
	size_t		strong = ((size_t *) ptr)[0];

	std_hint_assert_unchecked(strong != 0, STD_RC_FILE, 3781, 13);
	strong = std_wrapping_add_usize(strong, 1);
	((size_t *) ptr)[0] = strong;
	if (strong == 0)
		std_intrinsics_abort();
}

/* std_Rc_drop_frame_0_t is drop_slow's state across its pad: the block, the value's layout and drop. */
typedef struct std_Rc_drop_frame_0_t
{
	void	   *ptr;
	size_t		size;
	size_t		align;
	void		(*drop) (void *value);
} std_Rc_drop_frame_0_t;

/* std_Weak_drop is Weak's Drop, of drop_slow's _weak: the weak count one lower, and the block freed at zero. */
static void
std_Weak_drop(void *ptr, size_t size, size_t align)
{
	((size_t *) ptr)[1] = std_wrapping_sub_usize(((size_t *) ptr)[1], 1);
	if (((size_t *) ptr)[1] == 0)
	{
		size_t		inner_size = std_rc_inner_size(size, align);
		size_t		inner_align = std_rc_inner_align(align);

		std_Layout_for_value_raw_check(inner_size, inner_align);
		std_free(ptr, inner_size, inner_align);
	}
}

/* std_Rc_drop_body_0 is drop_slow: the value's drop in place, then _weak's drop. */
static void
std_Rc_drop_body_0(void *frame_)
{
	std_Rc_drop_frame_0_t *frame = frame_;

	if (frame->drop != NULL)
		frame->drop(std_Rc_deref(frame->ptr, frame->align));
	std_Weak_drop(frame->ptr, frame->size, frame->align);
}

/* std_Rc_drop_pad_0 is drop_slow's pad: _weak's drop should the value's drop unwind. */
static void
std_Rc_drop_pad_0(void *frame_)
{
	std_Rc_drop_frame_0_t *frame = frame_;

	std_Weak_drop(frame->ptr, frame->size, frame->align);
}

/*
 * std_Rc_drop is Rc's Drop: the strong count one lower, wrapping, and at
 * zero drop_slow, the value dropped and the implicit Weak with it.
 */
void
std_Rc_drop(void *ptr, size_t size, size_t align, void (*drop) (void *value))
{
	std_Rc_drop_frame_0_t frame = {.ptr = ptr, .size = size, .align = align, .drop = drop};

	((size_t *) ptr)[0] = std_wrapping_sub_usize(((size_t *) ptr)[0], 1);
	if (((size_t *) ptr)[0] == 0)
		std_landing(&frame, std_Rc_drop_body_0, std_Rc_drop_pad_0);
}

/* std_Rc_Debug_fmt is Rc's Debug: the value's own Debug. */
bool
std_Rc_Debug_fmt(void *ptr, size_t align, StdFmtFn value_fmt, StdFormatter *f)
{
	return value_fmt(std_Rc_deref(ptr, align), f);
}
