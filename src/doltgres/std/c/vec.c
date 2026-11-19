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
 * The C form of Rust's Vec<T> and String and of a slice's bounds checks (the
 * port text's sections 9 and 10), ported from alloc's vec/mod.rs and
 * raw_vec/mod.rs and core's slice/index.rs at Rust 1.99.1: each growth
 * reallocates where RawVec's does, and each panic has std's message and
 * location, the caller's for a #[track_caller] function and std's own file
 * and line otherwise.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/test.h"
#include "doltgres/std/vec.h"

/* RAW_VEC_FILE is raw_vec/mod.rs as Rust's panics place it. */
#define RAW_VEC_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "raw_vec" STD_SEP "mod.rs"

/* ALLOC_FILE is alloc/src/alloc.rs, where Global's checks report. */
#define ALLOC_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "alloc.rs"

/* VEC_FILE is vec/mod.rs as Rust's panics place it. */
#define VEC_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "vec" STD_SEP "mod.rs"

/* ALLOC_SLICE_FILE is alloc/src/slice.rs, where <[T]>::repeat's capacity check reports. */
#define ALLOC_SLICE_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "slice.rs"

/* SPEC_FROM_ITER_NESTED_FILE is alloc's vec/spec_from_iter_nested.rs, where a TrustedLen collect takes its capacity. */
#define SPEC_FROM_ITER_NESTED_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "vec" STD_SEP "spec_from_iter_nested.rs"

/* IN_PLACE_COLLECT_FILE is alloc's vec/in_place_collect.rs, the panic locations of from_iter_in_place. */
#define IN_PLACE_COLLECT_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "vec" STD_SEP "in_place_collect.rs"

/* IN_PLACE_DROP_FILE is alloc's vec/in_place_drop.rs, the panic location of InPlaceDrop's len. */
#define IN_PLACE_DROP_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "vec" STD_SEP "in_place_drop.rs"


/*
 * std_vec_max_size_for_align is Layout's max_size_for_align: the most bytes
 * an array of an alignment may take, isize::MAX less the alignment's
 * padding.
 */
static size_t
std_vec_max_size_for_align(size_t align)
{
	return (size_t) PG_INT64_MAX - (align - 1);
}

/* std_vec_capacity_overflow is RawVec's panic for a capacity past isize::MAX bytes. */
pg_noreturn static void
std_vec_capacity_overflow(void)
{
	std_panic(RAW_VEC_FILE, 28, 5, "capacity overflow", 17);
}

/*
 * std_vec_panic_numbers panics at a location with panic!'s formatted message
 * of two usizes, whose payload is a String: fmt is the message's template
 * with each usize's place written %zu, split there into its literal pieces.
 */
pg_noreturn void
std_vec_panic_numbers(const char *file, uint32 line, uint32 col, const char *fmt, size_t a,
					  size_t b)
{
	const char *first = strstr(fmt, "%zu");
	const char *second = strstr(first + 3, "%zu");
	StdFmtPiece pieces[] = {
		{fmt, (size_t) (first - fmt), 0, {0}}, STD_FMT_ARG(0),
		{first + 3, (size_t) (second - (first + 3)), 0, {0}}, STD_FMT_ARG(1),
		{second + 3, strlen(second + 3), 0, {0}},
	};
	StdFmtArg	args[] = {{&a, std_usize_Display_fmt}, {&b, std_usize_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_panic_fmt(file, line, col, &arguments);
}

/*
 * std_vec_min_non_zero_cap is RawVec's smallest capacity it allocates: 8 for 1-byte
 * elements, 4 up to 1 KB, and 1 above.
 */
size_t
std_vec_min_non_zero_cap(size_t size)
{
	if (size == 1)
		return 8;
	else if (size <= 1024)
		return 4;
	else
		return 1;
}

/*
 * std_vec_current_memory_checks is RawVecInner::current_memory's checks for a
 * buffer it has: unchecked_mul's and from_size_align_unchecked's
 * preconditions.
 */
static void
std_vec_current_memory_checks(const StdVec *v, size_t size, size_t align)
{
#ifdef USE_ASSERT_CHECKING
	size_t		alloc_size;

	if (!std_checked_mul_usize(size, v->cap, &alloc_size))
		STD_PTR_VIOLATED("usize::unchecked_mul cannot overflow", RAW_VEC_FILE, 639, 53);
	std_Layout_from_size_align_unchecked_check(alloc_size, align, RAW_VEC_FILE, 640, 30);
#endif
}

/* std_vec_allocated_checks is Global's checks of a block it got: NonNull::new's and cast_slice's new_unchecked. */
static void
std_vec_allocated_checks(const void *ptr)
{
	std_NonNull_new_unchecked_check(ptr, STD_PTR_NON_NULL_FILE, 282, 27);
	std_NonNull_new_unchecked_check(ptr, STD_PTR_NON_NULL_FILE, 1395, 18);
}

/* Vec's growth as the crates' code runs it, checked, and as std's own code does. */
#define STD_VEC_DEFINE
#define STD_VEC_UB_CHECKS 1
#define STD_VEC_SUFFIX Checked
#include "doltgres/std/vec_template.h"
#define STD_VEC_DEFINE
#define STD_VEC_UB_CHECKS 0
#define STD_VEC_SUFFIX Unchecked
#include "doltgres/std/vec_template.h"

/* std_vec_new is Vec::new, which allocates nothing; a zero-sized type's has room for usize::MAX. */
void
std_vec_new(StdVec *v, size_t size, size_t align)
{
	v->ptr = STD_DANGLING_FOR(align);
	v->len = 0;
	v->cap = size == 0 ? SIZE_MAX : 0;
}


/*
 * std_vec_from_elem_zeroed is vec![0; n] for an element whose zero is all
 * zero bytes: RawVec's with_capacity_zeroed, one zeroed allocation of n
 * elements, all of them in use.
 */
void
std_vec_from_elem_zeroed(StdVec *v, size_t n, size_t size, size_t align)
{
	std_vec_new(v, size, align);
	if (size != 0 && n > 0)
	{
		if (n > std_vec_max_size_for_align(align) / size)
			std_vec_capacity_overflow();
		v->ptr = std_Global_alloc_impl_Checked(n * size, align, true);
		if (v->ptr == NULL)
			std_handle_alloc_error(n * size, align);
		v->cap = n;
	}
	v->len = n;
}

/*
 * std_vec_u8_from_elem is vec![elem; n] of a u8, SpecFromElem's for u8: the
 * zeroed allocation for 0, and otherwise n bytes allocated and written with
 * the byte.
 */
void
std_vec_u8_from_elem(StdVec *v, uint8 elem, size_t n)
{
	if (elem == 0)
	{
		std_vec_from_elem_zeroed(v, n, 1, 1);
		return;
	}
	std_vec_with_capacity_Checked(v, n, 1, 1);
	std_ptr_write_bytes(v->ptr, elem, n, 1, 1);
	std_vec_set_len_Checked(v, n);
}


/*
 * std_vec_try_reserve is Vec::try_reserve: RawVec's amortized growth, false
 * for a capacity overflow or an allocation that fails, with nothing changed.
 */
bool
std_vec_try_reserve(StdVec *v, size_t additional, size_t size, size_t align)
{
	size_t		required;
	size_t		cap;
	void	   *moved;

	if (additional > v->cap - v->len)
	{
		if (size == 0 || additional > SIZE_MAX - v->len)
			return false;
		required = v->len + additional;
		cap = std_max_usize(v->cap * 2, required);
		cap = std_max_usize(std_vec_min_non_zero_cap(size), cap);
		if (cap > std_vec_max_size_for_align(align) / size)
			return false;
		if (v->cap != 0)
		{
			std_vec_current_memory_checks(v, size, align);
			std_hint_assert_unchecked(true, RAW_VEC_FILE, 556, 17);
			moved = std_Global_grow_impl_Checked(v->ptr, v->cap * size, align, cap * size, align, false);
		}
		else
			moved = std_Global_alloc_impl_Checked(cap * size, align, false);
		if (moved == NULL)
			return false;
		v->ptr = moved;
		v->cap = cap;
	}
	std_hint_assert_unchecked(!(additional > v->cap - v->len), RAW_VEC_FILE, 695, 13);
	return true;
}

/*
 * std_vec_try_reserve_exact is Vec::try_reserve_exact: growth to what is
 * needed, false for a capacity overflow or an allocation that fails, with
 * nothing changed.
 */
bool
std_vec_try_reserve_exact(StdVec *v, size_t additional, size_t size, size_t align)
{
	size_t		cap;
	void	   *moved;

	if (additional > v->cap - v->len)
	{
		if (size == 0 || additional > SIZE_MAX - v->len)
			return false;
		cap = v->len + additional;
		if (cap > std_vec_max_size_for_align(align) / size)
			return false;
		if (v->cap != 0)
		{
			std_vec_current_memory_checks(v, size, align);
			std_hint_assert_unchecked(true, RAW_VEC_FILE, 556, 17);
			moved = std_Global_grow_impl_Checked(v->ptr, v->cap * size, align, cap * size, align, false);
		}
		else
			moved = std_Global_alloc_impl_Checked(cap * size, align, false);
		if (moved == NULL)
			return false;
		v->ptr = moved;
		v->cap = cap;
	}
	std_hint_assert_unchecked(!(additional > v->cap - v->len), RAW_VEC_FILE, 730, 13);
	return true;
}

/*
 * std_vec_collect_first is SpecFromIterNested::from_iter's start for an
 * iterator of unknown length that gave a first element: a capacity of
 * max(MIN_NON_ZERO_CAP, lower + 1) by the rest's size_hint, and the element.
 */
void
std_vec_collect_first(StdVec *v, const void *elem, size_t lower, size_t size, size_t align)
{
	size_t		wanted = lower == SIZE_MAX ? SIZE_MAX : lower + 1;

	std_vec_with_capacity_Checked(v, std_max_usize(std_vec_min_non_zero_cap(size), wanted), size, align);
	if (size != 0)
		memcpy(v->ptr, elem, size);
	v->len = 1;
}

/*
 * std_vec_extend_one is one step of Vec::extend_desugared: when full, a
 * reserve of lower + 1 by the rest's size_hint, then the element.
 */
void
std_vec_extend_one(StdVec *v, const void *elem, size_t lower, size_t size, size_t align)
{
	if (v->len == v->cap)
		std_vec_reserve_Checked(v, lower == SIZE_MAX ? SIZE_MAX : lower + 1, size, align);
	if (size != 0)
		memcpy((char *) v->ptr + v->len * size, elem, size);
	v->len++;
}

/*
 * std_vec_split_off is Vec::split_off: the elements from at on moved into a
 * new vector of exactly their count, panicking at its caller for an index
 * past the length.
 */
void
std_vec_split_off(StdVec *v, size_t at, size_t size, size_t align, StdVec *result, const char *file,
				  uint32 line, uint32 col)
{
	size_t		other_len;

	if (at > v->len)
		std_vec_panic_numbers(file, line, col, "`at` split index (is %zu) should be <= len (is %zu)", at, v->len);
	other_len = v->len - at;
	std_vec_with_capacity_Checked(result, other_len, size, align);
	v->len = at;
	result->len = other_len;
	std_ptr_copy_nonoverlapping_Checked((char *) v->ptr + at * size, result->ptr, other_len, size, align);
}

/*
 * std_vec_pop is Vec::pop: the last element moved out, or nothing for an
 * empty vector, its assert_unchecked that the length is within the capacity
 * checked as the crates' code runs it.
 */
bool
std_vec_pop(StdVec *v, void *out, size_t size)
{
	if (v->len == 0)
		return false;
	v->len--;
	std_hint_assert_unchecked(v->len < v->cap, VEC_FILE, 2878, 17);
	if (size != 0)
		memcpy(out, (char *) v->ptr + v->len * size, size);
	return true;
}

/*
 * std_vec_clone is Vec::clone of a Copy element type: to_vec's one
 * allocation of exactly the length, and for a length above zero a copy and
 * set_len.
 */
void
std_vec_clone(StdVec *result, const StdVec *v, size_t size, size_t align)
{
	std_vec_with_capacity_Checked(result, v->len, size, align);
	if (v->len > 0)
	{
		std_ptr_copy_nonoverlapping_Checked(v->ptr, result->ptr, v->len, size, align);
		std_vec_set_len_Checked(result, v->len);
	}
}

/*
 * std_vec_dedup_by_frame_0_t is dedup_by's state across its pad: its
 * arguments, and its FillGapOnDrop's read and write places while it is live.
 */
typedef struct std_vec_dedup_by_frame_0_t
{
	StdVec	   *v;
	size_t		size;
	size_t		align;
	bool		(*same_bucket) (void *captures, void *a, void *b);
	void	   *captures;
	void		(*drop) (void *elem);
	size_t		read;
	size_t		write;
	bool		gap_live;
} std_vec_dedup_by_frame_0_t;

/* std_vec_dedup_by_body_0 is dedup_by's body. */
static void
std_vec_dedup_by_body_0(void *frame_)
{
	std_vec_dedup_by_frame_0_t *frame = frame_;
	size_t		size = frame->size;
	size_t		len = frame->v->len;
	void	   *start = frame->v->ptr;
	size_t		first_duplicate_idx = 1;

	if (len <= 1)
		return;
	while (first_duplicate_idx != len)
	{
		if (frame->same_bucket(frame->captures, STD_PTR_AT(start, first_duplicate_idx, size),
							   STD_PTR_AT(start, std_wrapping_sub_usize(first_duplicate_idx, 1), size)))
			break;
		first_duplicate_idx++;
	}
	if (first_duplicate_idx == len)
		return;
	frame->read = first_duplicate_idx + 1;
	frame->write = first_duplicate_idx;
	frame->gap_live = true;
	if (frame->drop != NULL)
		frame->drop(STD_PTR_AT(start, first_duplicate_idx, size));
	while (frame->read < len)
	{
		void	   *read_ptr = STD_PTR_AT(start, frame->read, size);
		void	   *prev_ptr = STD_PTR_AT(start, std_wrapping_sub_usize(frame->write, 1), size);

		if (frame->same_bucket(frame->captures, read_ptr, prev_ptr))
		{
			frame->read++;
			if (frame->drop != NULL)
				frame->drop(read_ptr);
		}
		else
		{
			std_ptr_copy_nonoverlapping_Checked(read_ptr, STD_PTR_AT(start, frame->write, size), 1, size, frame->align);
			frame->write++;
			frame->read++;
		}
	}
	frame->gap_live = false;
	std_vec_set_len_Checked(frame->v, frame->write);
}

/*
 * std_vec_dedup_by_pad_0 is dedup_by's pad, FillGapOnDrop's drop: the
 * elements not yet read moved down over the gap, and the length set to the
 * elements kept.
 */
static void
std_vec_dedup_by_pad_0(void *frame_)
{
	std_vec_dedup_by_frame_0_t *frame = frame_;

	if (frame->gap_live)
	{
		void	   *ptr = frame->v->ptr;
		size_t		len = frame->v->len;
		size_t		items_left = std_wrapping_sub_usize(len, frame->read);

		std_ptr_copy(STD_PTR_AT(ptr, frame->read, frame->size), STD_PTR_AT(ptr, frame->write, frame->size),
					 items_left, frame->size, frame->align);
		std_vec_set_len_Checked(frame->v, len - std_wrapping_sub_usize(frame->read, frame->write));
	}
}

/*
 * std_vec_dedup_by is Vec::dedup_by: each element that same_bucket finds
 * the same as the last one kept dropped, the rest moved down in order, the
 * elements not yet read kept should same_bucket panic.
 */
void
std_vec_dedup_by(StdVec *v, size_t size, size_t align, bool (*same_bucket) (void *captures, void *a, void *b),
				 void *captures, void (*drop) (void *elem))
{
	std_vec_dedup_by_frame_0_t frame = {.v = v, .size = size, .align = align, .same_bucket = same_bucket, .captures = captures,
	.drop = drop};

	std_landing(&frame, std_vec_dedup_by_body_0, std_vec_dedup_by_pad_0);
}

/* std_vec_dedup_eq_t is dedup's closure's capture: the element type's PartialEq. */
typedef struct std_vec_dedup_eq_t
{
	bool		(*eq) (const void *self, const void *other);
} std_vec_dedup_eq_t;

/* std_vec_dedup_closure_0 is dedup's |a, b| a == b. */
static bool
std_vec_dedup_closure_0(void *captures, void *a, void *b)
{
	std_vec_dedup_eq_t *eq = captures;

	return eq->eq(a, b);
}

/* std_vec_dedup is Vec::dedup: dedup_by with the elements' PartialEq. */
void
std_vec_dedup(StdVec *v, size_t size, size_t align, bool (*eq) (const void *self, const void *other),
			  void (*drop) (void *elem))
{
	std_vec_dedup_eq_t captures = {eq};

	std_vec_dedup_by(v, size, align, std_vec_dedup_closure_0, &captures, drop);
}

/*
 * std_slice_to_vec_clone_frame_0_t is to_vec's state across its pad: its
 * arguments, and its DropGuard's count of the clones written while the
 * vector it fills is live.
 */
typedef struct std_slice_to_vec_clone_frame_0_t
{
	StdVec	   *result;
	const void *s;
	size_t		len;
	size_t		size;
	size_t		align;
	void		(*clone) (const void *elem, void *result);
	void		(*drop) (void *elem);
	size_t		num_init;
	bool		guard_live;
} std_slice_to_vec_clone_frame_0_t;

/* std_slice_to_vec_clone_body_0 is to_vec's body. */
static void
std_slice_to_vec_clone_body_0(void *frame_)
{
	std_slice_to_vec_clone_frame_0_t *frame = frame_;

	std_vec_with_capacity_Checked(frame->result, frame->len, frame->size, frame->align);
	frame->guard_live = true;
	for (size_t i = 0, i_len = frame->len; i < i_len; i++)
	{
		frame->num_init = i;
		frame->clone(STD_PTR_AT(frame->s, i, frame->size), STD_PTR_AT(frame->result->ptr, i, frame->size));
	}
	frame->guard_live = false;
	std_vec_set_len_Checked(frame->result, frame->len);
}

/*
 * std_slice_to_vec_clone_pad_0 is to_vec's pad: its DropGuard's set_len to
 * the clones written, then the vector's drop.
 */
static void
std_slice_to_vec_clone_pad_0(void *frame_)
{
	std_slice_to_vec_clone_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		std_vec_set_len_Checked(frame->result, frame->num_init);
		std_vec_drop_Checked(frame->result, frame->size, frame->align, frame->drop);
	}
}

/*
 * std_slice_to_vec_clone is <[T]>::to_vec of a Clone T, ConvertVec's: a
 * vector of exactly the length, each element's clone written in order, and
 * the length set once all are, the clones written so far dropped should one
 * unwind.
 */
void
std_slice_to_vec_clone(StdVec *result, const void *s, size_t len, size_t size, size_t align,
					   void (*clone) (const void *elem, void *result), void (*drop) (void *elem))
{
	std_slice_to_vec_clone_frame_0_t frame = {.result = result, .s = s, .len = len, .size = size, .align = align,
	.clone = clone, .drop = drop};

	std_landing(&frame, std_slice_to_vec_clone_body_0, std_slice_to_vec_clone_pad_0);
}

/*
 * std_vec_InPlaceDstDataSrcBufDrop_t is InPlaceDstDataSrcBufDrop: the
 * written elements and the source's allocation, which its drop frees after
 * the elements, should one's drop panic too.
 */
typedef struct std_vec_InPlaceDstDataSrcBufDrop_t
{
	void	   *ptr;
	size_t		len;
	size_t		dst_size;
	void		(*dst_drop) (void *elem);
	size_t		src_cap;
	size_t		src_size;
	size_t		align;
} std_vec_InPlaceDstDataSrcBufDrop_t;

/* std_vec_InPlaceDstDataSrcBufDrop_drop_body_0 is InPlaceDstDataSrcBufDrop's drop of the written elements. */
static void
std_vec_InPlaceDstDataSrcBufDrop_drop_body_0(void *frame_)
{
	std_vec_InPlaceDstDataSrcBufDrop_t *frame = frame_;

	if (frame->dst_drop != NULL)
		std_ptr_drop_in_place_slice(frame->ptr, frame->len, frame->dst_size, frame->dst_drop);
}

/* std_vec_InPlaceDstDataSrcBufDrop_drop_pad_0 is its pad: the allocation, the `_drop_allocation` RawVec's drop. */
static void
std_vec_InPlaceDstDataSrcBufDrop_drop_pad_0(void *frame_)
{
	std_vec_InPlaceDstDataSrcBufDrop_t *frame = frame_;
	StdVec		allocation = {frame->ptr, 0, frame->src_cap};

	std_vec_free_Checked(&allocation, frame->src_size, frame->align);
}

/*
 * std_vec_InPlaceDstDataSrcBufDrop_drop is InPlaceDstDataSrcBufDrop's drop:
 * the written elements dropped, then the source's allocation freed at the
 * end of its scope, or as an element's drop unwinds.
 */
static void
std_vec_InPlaceDstDataSrcBufDrop_drop(std_vec_InPlaceDstDataSrcBufDrop_t *self)
{
	std_landing(self, std_vec_InPlaceDstDataSrcBufDrop_drop_body_0, std_vec_InPlaceDstDataSrcBufDrop_drop_pad_0);
	std_vec_InPlaceDstDataSrcBufDrop_drop_pad_0(self);
}

/*
 * std_vec_from_iter_in_place_map_frame_0_t is from_iter_in_place's state
 * across its pad: the source's IntoIter, which the Map holds, the written
 * elements its InPlaceDrop drops, and the buffer its
 * InPlaceDstDataSrcBufDrop frees.
 */
typedef struct std_vec_from_iter_in_place_map_frame_0_t
{
	StdVecIntoIter iter;
	bool		iter_live;
	size_t		dst_size;
	void		(*dst_drop) (void *elem);
	void		(*f) (void *elem, void *result);
	StdVec	   *result;
	void	   *dst_buf;
	void	   *dst;
	bool		sink_live;
	size_t		len;
	size_t		src_cap;
	bool		dst_guard_live;
} std_vec_from_iter_in_place_map_frame_0_t;

/* std_vec_from_iter_in_place_map_body_0 is from_iter_in_place's body. */
static void
std_vec_from_iter_in_place_map_body_0(void *frame_)
{
	std_vec_from_iter_in_place_map_frame_0_t *frame = frame_;
	size_t		src_size = frame->iter.size;
	size_t		align = frame->iter.align;
	size_t		dst_cap;
	StdVec		remaining;

	frame->src_cap = frame->iter.buf.cap;
	frame->dst_buf = frame->iter.buf.ptr;
	std_usize_unchecked_mul_check(frame->src_cap, src_size, IN_PLACE_COLLECT_FILE, 263, 23);
	dst_cap = frame->src_cap * src_size / frame->dst_size;
	frame->dst = frame->dst_buf;
	frame->sink_live = true;
	while (frame->iter.next != frame->iter.buf.len)
	{
		void	   *elem = STD_PTR_AT(frame->iter.buf.ptr, frame->iter.next, src_size);

		frame->iter.next++;
		frame->f(elem, frame->dst);
		frame->dst = STD_PTR_AT(frame->dst, 1, frame->dst_size);
	}
	frame->sink_live = false;
	frame->len = std_ptr_offset_from_unsigned_Checked(frame->dst, frame->dst_buf, frame->dst_size,
													  IN_PLACE_COLLECT_FILE, 378, 46);
	frame->dst_guard_live = true;
	remaining.ptr = STD_PTR_AT(frame->iter.buf.ptr, frame->iter.next, src_size);
	remaining.len = std_VecIntoIter_len(&frame->iter);
	frame->iter.buf.cap = 0;
	frame->iter.buf.ptr = STD_DANGLING_FOR(align);
	frame->iter.buf.len = 0;
	frame->iter.next = 0;
	if (frame->iter.drop != NULL)
		std_ptr_drop_in_place_slice(remaining.ptr, remaining.len, src_size, frame->iter.drop);
	if (frame->dst_size != 0 && src_size % frame->dst_size != 0 && frame->src_cap > 0 &&
		frame->src_cap * src_size != dst_cap * frame->dst_size)
	{
		size_t		old_size;
		size_t		new_size;

		std_usize_unchecked_mul_check(src_size, frame->src_cap, IN_PLACE_COLLECT_FILE, 312, 48);
		old_size = src_size * frame->src_cap;
		std_Layout_from_size_align_unchecked_check(old_size, align, IN_PLACE_COLLECT_FILE, 313, 30);
		std_usize_unchecked_mul_check(frame->dst_size, dst_cap, IN_PLACE_COLLECT_FILE, 318, 43);
		new_size = frame->dst_size * dst_cap;
		std_Layout_from_size_align_unchecked_check(new_size, align, IN_PLACE_COLLECT_FILE, 319, 30);
		if (new_size == 0)
		{
			std_free(frame->dst_buf, old_size, align);
			frame->dst_buf = STD_DANGLING_FOR(align);
		}
		else
		{
			void	   *reallocated;

			std_hint_assert_unchecked(new_size <= old_size, ALLOC_FILE, 398, 17);
			reallocated = std_global_realloc(frame->dst_buf, old_size, align, new_size);
			if (reallocated == NULL)
				std_handle_alloc_error(new_size, align);
			frame->dst_buf = reallocated;
		}
	}
	frame->dst_guard_live = false;
#ifdef USE_ASSERT_CHECKING
	if (!(frame->len <= dst_cap))
		STD_PTR_VIOLATED("Vec::from_parts_in requires that length <= capacity", VEC_FILE, 1306, 9);
#endif
	frame->result->ptr = frame->dst_buf;
	frame->result->len = frame->len;
	frame->result->cap = dst_cap;
	frame->iter_live = false;
	std_VecIntoIter_drop(&frame->iter);
}

/*
 * std_vec_from_iter_in_place_map_pad_0 is from_iter_in_place's pad: the
 * written elements and the source's buffer by InPlaceDrop, or by
 * InPlaceDstDataSrcBufDrop once the reading is done, then the IntoIter.
 */
static void
std_vec_from_iter_in_place_map_pad_0(void *frame_)
{
	std_vec_from_iter_in_place_map_frame_0_t *frame = frame_;

	if (frame->sink_live && frame->dst_drop != NULL)
		std_ptr_drop_in_place_slice(frame->dst_buf,
									std_ptr_offset_from_unsigned_Checked(frame->dst, frame->dst_buf,
																		 frame->dst_size, IN_PLACE_DROP_FILE, 16,
																		 27),
									frame->dst_size, frame->dst_drop);
	if (frame->dst_guard_live)
	{
		std_vec_InPlaceDstDataSrcBufDrop_t dst_guard = {frame->dst_buf, frame->len, frame->dst_size,
		frame->dst_drop, frame->src_cap, frame->iter.size, frame->iter.align};

		std_vec_InPlaceDstDataSrcBufDrop_drop(&dst_guard);
	}
	if (frame->iter_live)
		std_VecIntoIter_drop(&frame->iter);
}

/*
 * std_vec_from_iter_in_place_map is in_place_collect.rs' from_iter_in_place
 * of a Map over a vec::IntoIter, taking the source vector, whose element
 * types share an alignment: each mapped element written over the read ones
 * by collect_in_place's try_fold, the unread ones dropped and the
 * allocation taken from the IntoIter, a shrink where needs_realloc finds the
 * sizes leave a remainder, and the vector of the written ones.
 */
void
std_vec_from_iter_in_place_map(StdVec *src, size_t src_size, size_t align, void (*src_drop) (void *elem),
							   size_t dst_size, void (*dst_drop) (void *elem), void (*f) (void *elem, void *result),
							   StdVec *result)
{
	std_vec_from_iter_in_place_map_frame_0_t frame = {.iter = std_Vec_IntoIterator_into_iter(src, src_size, align,
																							 src_drop),
	.iter_live = true, .dst_size = dst_size, .dst_drop = dst_drop, .f = f, .result = result};

	std_landing(&frame, std_vec_from_iter_in_place_map_body_0, std_vec_from_iter_in_place_map_pad_0);
}

/*
 * std_vec_with_capacity_trusted is SpecFromIterNested's from_iter's start
 * for a TrustedLen iterator's size_hint: a vector of its upper bound's
 * capacity, and "capacity overflow" for one without one.
 */
void
std_vec_with_capacity_trusted(StdVec *v, StdSizeHint hint, size_t size, size_t align)
{
	if (!hint.has_upper)
	{
		static const char text[] = "capacity overflow";

		std_panic(SPEC_FROM_ITER_NESTED_FILE, 57, 18, text, sizeof(text) - 1);
	}
	std_vec_with_capacity_Checked(v, hint.upper, size, align);
}

/*
 * std_vec_extend_trusted_reserve is Vec's extend_trusted up to its writes:
 * room reserved for a TrustedLen iterator's upper bound, and "capacity
 * overflow" for one without one; the caller writes the items and the length.
 */
void
std_vec_extend_trusted_reserve(StdVec *v, StdSizeHint hint, size_t size, size_t align)
{
	if (!hint.has_upper)
	{
		static const char text[] = "capacity overflow";

		std_panic(VEC_FILE, 4146, 13, text, sizeof(text) - 1);
	}
	std_vec_reserve_Checked(v, hint.upper, size, align);
}


/* std_vec_insert is Vec::insert, which panics past the length at its caller. */
void
std_vec_insert(StdVec *v, size_t index, const void *elem, size_t size, size_t align,
			   const char *file, uint32 line, uint32 col)
{
	char	   *at;

	if (index > v->len)
		std_vec_panic_numbers(file, line, col,
							  "insertion index (is %zu) should be <= len (is %zu)",
							  index, v->len);
	if (v->len == v->cap)
		std_vec_grow_amortized_Checked(v, 1, size, align);
	if (size != 0)
	{
		at = (char *) v->ptr + index * size;
		if (index < v->len)
			std_ptr_copy(at, at + size, v->len - index, size, align);
		memcpy(at, elem, size);
	}
	v->len++;
}

/* std_vec_remove is Vec::remove, which panics at or past the length at its caller. */
void
std_vec_remove(StdVec *v, size_t index, void *out, size_t size, size_t align, const char *file, uint32 line, uint32 col)
{
	char	   *at;

	if (index >= v->len)
		std_vec_panic_numbers(file, line, col,
							  "removal index (is %zu) should be < len (is %zu)",
							  index, v->len);
	if (size != 0)
	{
		at = (char *) v->ptr + index * size;
		memcpy(out, at, size);
		std_ptr_copy(at + size, at, v->len - index - 1, size, align);
	}
	v->len--;
}

/*
 * std_ptr_drop_in_place_slice is drop_in_place of a slice of elements whose
 * drop does something, NULL for one whose drop does nothing: each element's
 * drop in order, the rest still dropped should one panic, as a slice's drop
 * glue does.
 */
void
std_ptr_drop_in_place_slice(void *ptr, size_t len, size_t size, void (*drop) (void *))
{
	volatile size_t i = 0;
	StdUnwind	unwind;

	if (drop == NULL)
		return;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		while (i < len)
		{
			void	   *elem = (char *) ptr + i * size;

			i++;
			drop(elem);
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		while (i < len)
		{
			void	   *elem = (char *) ptr + i * size;

			i++;
			drop(elem);
		}
		std_unwind_resume(&unwind);
	}
}


/*
 * std_vec_truncate is Vec::truncate: the elements past a length dropped, the
 * length shortened first so a panicking drop drops none twice; nothing for a
 * length past the vector's.
 */
void
std_vec_truncate(StdVec *v, size_t len, size_t size, void (*drop) (void *))
{
	size_t		remaining_len;

	if (len > v->len)
		return;
	remaining_len = v->len - len;
	v->len = len;
	std_ptr_drop_in_place_slice((char *) v->ptr + len * size, remaining_len, size, drop);
}

/*
 * std_vec_resize is Vec::resize of a value whose clone is a copy:
 * extend_with's reserve and its copies up to the new length, else truncate.
 */
void
std_vec_resize(StdVec *v, size_t new_len, const void *value, size_t size, size_t align)
{
	size_t		len = v->len;

	if (new_len > len)
	{
		size_t		n = new_len - len;

		std_vec_reserve_Checked(v, n, size, align);
		for (size_t i = 0; i < n; i++)
			memcpy((char *) v->ptr + (len + i) * size, value, size);
		v->len = new_len;
	}
	else
		std_vec_truncate(v, new_len, size, NULL);
}

/*
 * std_slice_concat is <[V]>::concat for a V that borrows as [T]: the sum of
 * the slices' lengths, with Sum's overflow check, one allocation of it, and
 * each slice's elements extended in turn. A slice's element is stride bytes
 * that start with the slice's pointer and length, as a &[T] and a Vec<T> do.
 */
void
std_slice_concat(const void *slices, size_t n, size_t stride, size_t size, size_t align, StdVec *result)
{
	size_t		total = 0;
	volatile bool result_live = false;
	StdUnwind	unwind;

	for (size_t i = 0; i < n; i++)
		total = std_add_usize(total, ((const StdBytes *) ((const char *) slices + i * stride))->len, STD_ACCUM_FILE,
							  206, 1);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_with_capacity_Checked(result, total, size, align);
		result_live = true;
		for (size_t i = 0; i < n; i++)
		{
			const StdBytes *v = (const StdBytes *) ((const char *) slices + i * stride);

			std_vec_extend_from_slice_Checked(result, v->ptr, v->len, size, align);
		}
		result_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (result_live)
			std_vec_free_Checked(result, size, align);
		std_unwind_resume(&unwind);
	}
}

/* std_slice_repeat_frame_0_t is repeat's state across its pad: its arguments and the buffer it fills. */
typedef struct std_slice_repeat_frame_0_t
{
	const void *slice;
	size_t		len;
	size_t		size;
	size_t		align;
	size_t		n;
	StdVec	   *buf;
	bool		buf_live;
} std_slice_repeat_frame_0_t;

/* std_slice_repeat_body_0 is repeat's body past n's zero case: the capacity, one copy, the doublings, and the rest. */
static void
std_slice_repeat_body_0(void *frame_)
{
	std_slice_repeat_frame_0_t *frame = frame_;
	size_t		capacity;
	size_t		rem_len;

	if (!std_checked_mul_usize(frame->len, frame->n, &capacity))
		std_option_expect_failed("capacity overflow", sizeof("capacity overflow") - 1, ALLOC_SLICE_FILE, 524, 50);
	std_vec_with_capacity_Checked(frame->buf, capacity, frame->size, frame->align);
	frame->buf_live = true;
	std_vec_extend_from_slice_Checked(frame->buf, frame->slice, frame->len, frame->size, frame->align);
	for (size_t m = frame->n >> 1; m > 0; m >>= 1)
	{
		size_t		buf_len;

		std_ptr_copy_nonoverlapping_Checked(frame->buf->ptr, STD_PTR_AT(frame->buf->ptr, frame->buf->len, frame->size),
											frame->buf->len, frame->size, frame->align);
		buf_len = frame->buf->len;
		std_vec_set_len_Checked(frame->buf, buf_len * 2);
	}
	rem_len = capacity - frame->buf->len;
	if (rem_len > 0)
	{
		std_ptr_copy_nonoverlapping_Checked(frame->buf->ptr, STD_PTR_AT(frame->buf->ptr, frame->buf->len, frame->size),
											rem_len, frame->size, frame->align);
		std_vec_set_len_Checked(frame->buf, capacity);
	}
	frame->buf_live = false;
}

/* std_slice_repeat_pad_0 is repeat's pad: the buffer while it is live. */
static void
std_slice_repeat_pad_0(void *frame_)
{
	std_slice_repeat_frame_0_t *frame = frame_;

	if (frame->buf_live)
		std_vec_free_Checked(frame->buf, frame->size, frame->align);
}

/*
 * std_slice_repeat is <[T]>::repeat for a Copy T: a vector of the slice n
 * times, its capacity the product with "capacity overflow"'s check, filled
 * by one copy of the slice and doublings of what it holds, then the rest.
 */
void
std_slice_repeat(const void *slice, size_t len, size_t size, size_t align, size_t n, StdVec *result)
{
	std_slice_repeat_frame_0_t frame = {.slice = slice, .len = len, .size = size, .align = align, .n = n,
	.buf = result};

	if (n == 0)
	{
		std_vec_new(result, size, align);
		return;
	}
	std_landing(&frame, std_slice_repeat_body_0, std_slice_repeat_pad_0);
}

/* std_str_repeat is str::repeat: its bytes repeated n times, as a String. */
void
std_str_repeat(const char *s, size_t len, size_t n, StdString *result)
{
	std_slice_repeat(s, len, 1, 1, n, result);
}

/*
 * std_vec_clear is Vec::clear: the length set to zero, then every element
 * dropped, the length set first so a panicking drop drops none twice.
 */
void
std_vec_clear(StdVec *v, size_t size, void (*drop) (void *))
{
	size_t		len = v->len;

	v->len = 0;
	std_ptr_drop_in_place_slice(v->ptr, len, size, drop);
}

/*
 * std_vec_drain_full is Vec::drain of the full range `..`, whose
 * slice::range can't fail, so rustc's instance has no range check: the vector's
 * length cut to 0, and a Drain over every element that restores the empty
 * tail when dropped.
 */
StdVecDrain
std_vec_drain_full(StdVec *v, size_t size, size_t align, void (*drop) (void *))
{
	size_t		len = v->len;
	StdVecDrain drain;

	v->len = 0;
	std_slice_from_raw_parts_check(v->ptr, len, size, align, VEC_FILE, 3042, 31);
	drain.vec = v;
	drain.next = 0;
	drain.end = len;
	drain.tail_start = len;
	drain.tail_len = 0;
	drain.size = size;
	drain.align = align;
	drain.drop = drop;
	return drain;
}

/*
 * std_vec_drain is Vec::drain of a range, whose slice::range panics as
 * into_slice_range does: the vector's length cut to the range's start, and a
 * Drain over the range that moves the tail back when dropped.
 */
StdVecDrain
std_vec_drain(StdVec *v, StdBound start, StdBound end, size_t size, size_t align, void (*drop) (void *))
{
	size_t		len = v->len;
	size_t		range_start;
	size_t		range_end;
	StdVecDrain drain;

	std_slice_range(start, end, len, &range_start, &range_end);
	v->len = range_start;
	std_slice_from_raw_parts_check((char *) v->ptr + range_start * size, range_end - range_start, size, align, VEC_FILE,
								   3042, 31);
	drain.vec = v;
	drain.next = range_start;
	drain.end = range_end;
	drain.tail_start = range_end;
	drain.tail_len = len - range_end;
	drain.size = size;
	drain.align = align;
	drain.drop = drop;
	return drain;
}

/* std_VecDrain_Iterator_next is Drain's next: the next element of the range, moved out, or false at its end. */
bool
std_VecDrain_Iterator_next(void *self_, void *result)
{
	StdVecDrain *self = (StdVecDrain *) self_;

	if (self->next == self->end)
		return false;
	memcpy(result, (char *) self->vec->ptr + self->next * self->size, self->size);
	self->next++;
	return true;
}

/*
 * std_VecDrain_DropGuard_drop is Drain's DropGuard's drop: the untouched tail
 * moved back to the vector's length, and the length restored to cover it.
 */
static void
std_VecDrain_DropGuard_drop(StdVecDrain *drain)
{
	if (drain->tail_len > 0)
	{
		StdVec	   *source_vec = drain->vec;
		size_t		start = source_vec->len;
		size_t		tail = drain->tail_start;

		if (tail != start)
			std_ptr_copy((char *) source_vec->ptr + tail * drain->size,
						 (char *) source_vec->ptr + start * drain->size, drain->tail_len, drain->size, drain->align);
		source_vec->len = start + drain->tail_len;
	}
}

/*
 * std_VecDrain_drop is Drain's drop: the elements not yielded dropped, then
 * the tail moved back by the DropGuard, which runs even when one of those
 * drops panics.
 */
void
std_VecDrain_drop(void *self_)
{
	StdVecDrain *self = (StdVecDrain *) self_;
	size_t		drop_start = self->next;
	size_t		drop_len = self->end - self->next;
	volatile bool guard_live;
	StdUnwind	unwind;

	self->next = self->end;
	if (self->size == 0)
	{
		StdVec	   *vec = self->vec;
		size_t		old_len = vec->len;

		vec->len = old_len + drop_len + self->tail_len;
		std_vec_truncate(vec, old_len + self->tail_len, 0, self->drop);
		return;
	}
	guard_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		if (drop_len != 0)
			std_ptr_drop_in_place_slice((char *) self->vec->ptr + drop_start * self->size, drop_len, self->size,
										self->drop);
		guard_live = false;
		std_VecDrain_DropGuard_drop(self);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (guard_live)
		{
			guard_live = false;
			std_VecDrain_DropGuard_drop(self);
		}
		std_unwind_resume(&unwind);
	}
}

#ifdef DOLTGRES_TEST_HOST

/*
 * test_std_vec_tests_an_over_aligned_element_stays_aligned checks a vector
 * of 64-byte-aligned elements: its empty pointer is dangling at 64, as
 * NonNull::dangling is, and each growth keeps the buffer at 64.
 */
void
test_std_vec_tests_an_over_aligned_element_stays_aligned(void)
{
	StdVec		v;
	StdTestsAligned64 elem = {{0}};

	std_vec_new(&v, sizeof(StdTestsAligned64), _Alignof(StdTestsAligned64));
	if (v.ptr != (void *) (uintptr_t) 64)
		std_panic(__FILE__, __LINE__, 0, "an empty vector's pointer is not dangling at 64", sizeof("an empty vector's pointer is not dangling at 64") - 1);
	for (int i = 0; i < 9; i++)
	{
		elem.bytes[0] = (uint8) i;
		std_vec_push_Checked(&v, &elem, sizeof(StdTestsAligned64), _Alignof(StdTestsAligned64));
		if ((uintptr_t) v.ptr % 64 != 0)
			std_panic(__FILE__, __LINE__, 0, "a grown vector's buffer is not aligned to 64", sizeof("a grown vector's buffer is not aligned to 64") - 1);
	}
	for (int i = 0; i < 9; i++)
		if (STD_VEC_AT(&v, StdTestsAligned64, i).bytes[0] != (uint8) i)
			std_panic(__FILE__, __LINE__, 0, "a moved element changed", sizeof("a moved element changed") - 1);
	std_vec_free_Checked(&v, sizeof(StdTestsAligned64), _Alignof(StdTestsAligned64));
}

#endif							/* DOLTGRES_TEST_HOST */

/* std_vec_swap_remove is Vec::swap_remove, which panics at or past the length in std. */
void
std_vec_swap_remove(StdVec *v, size_t index, void *out, size_t size, size_t align)
{
	char	   *at;

	if (index >= v->len)
		std_vec_panic_numbers(VEC_FILE, 2258, 13,
							  "swap_remove index (is %zu) should be < len (is %zu)",
							  index, v->len);
	if (size != 0)
	{
		at = (char *) v->ptr + index * size;
		memcpy(out, at, size);
		std_ptr_copy((char *) v->ptr + (v->len - 1) * size, at, 1, size, align);
	}
	v->len--;
}

/* std_vec_PartialEq_eq is Vec's PartialEq: whether two vectors of elements of a size hold equal elements in order. */
bool
std_vec_PartialEq_eq(const StdVec *self, const StdVec *other, size_t size, bool (*eq) (const void *a, const void *b))
{
	if (self->len != other->len)
		return false;
	for (size_t i = 0; i < self->len; i++)
		if (!eq(STD_VEC_AT_SIZE(self, i, size), STD_VEC_AT_SIZE(other, i, size)))
			return false;
	return true;
}
