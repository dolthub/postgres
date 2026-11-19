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
 * The template of core's sort tests' known_good_stable_sort.rs
 * (library/alloctests/tests/sort, Rust 1.99.1): a simple top-down merge
 * sort the tests check the sort under test against, at an element type. An
 * instance defines its parameters and includes this file; every parameter
 * is undefined at the end. STD_KG_TS spells T, STD_KG_T is T, and
 * STD_KG_LT is T's PartialOrd::lt on pointers; the includer instantiates
 * slice_template.h at the same STD_KG_TS, whose swap it calls.
 */

#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"

/* STD_KG_CAT pastes two tokens after expanding them. */
#define STD_KG_CAT_(a, b) a##b
#define STD_KG_CAT(a, b) STD_KG_CAT_(a, b)

/* STD_KG_NAME names the instance's functions. */
#define STD_KG_NAME(name) STD_KG_CAT(alloctests_sort_known_good_stable_sort_##name##_, STD_KG_TS)

/* STD_KG_FILE is known_good_stable_sort.rs, where its checks report. */
#define STD_KG_FILE STD_RUST_LIBRARY STD_SEP "alloctests" STD_SEP "tests" STD_SEP "sort" STD_SEP "known_good_stable_sort.rs"

/*
 * The instance's merge merges the sorted halves of v split at mid through
 * the scratch buffer, taking the left element on a tie, which keeps the
 * sort stable.
 */
static void
STD_KG_NAME(merge) (STD_KG_T * v, size_t len, STD_KG_T * scratch_ptr, StdSortLess *is_less, size_t mid)
{
	size_t		l = 0;
	size_t		r = mid;
	const		STD_KG_T *copy_ptr;
	size_t		i;

#ifdef USE_ASSERT_CHECKING
	if (!(mid > 0 && mid < len))
	{
		static const char text[] = "assertion failed: mid > 0 && mid < len";

		std_panic(STD_KG_FILE, 105, 5, text, sizeof(text) - 1);
	}
#endif
	for (size_t k = 0, k_end = len; k < k_end; k = std_forward_unchecked_usize_Checked(k, 1))
	{
		const		STD_KG_T *left_ptr = &v[l];
		const		STD_KG_T *right_ptr = &v[r];
		bool		is_lt = !is_less->f(is_less->captures, right_ptr, left_ptr);

		copy_ptr = is_lt ? left_ptr : right_ptr;
		std_ptr_copy_nonoverlapping_Checked(copy_ptr, &scratch_ptr[k], 1, sizeof(STD_KG_T), _Alignof(STD_KG_T));
		l = std_add_usize(l, (size_t) is_lt, STD_KG_FILE, 126, 13);
		r = std_add_usize(r, (size_t) !is_lt, STD_KG_FILE, 127, 13);
		if (std_add_u8((uint8) (l == mid), (uint8) (r == len), STD_KG_FILE, 130, 17) != 0)
			break;
	}
	copy_ptr = l == mid ? &v[r] : &v[l];
	i = std_add_usize(l, std_sub_usize(r, mid, STD_KG_FILE, 137, 22), STD_KG_FILE, 137, 17);
	std_ptr_copy_nonoverlapping_Checked(copy_ptr, &scratch_ptr[i], std_sub_usize(len, i, STD_KG_FILE, 138, 64),
										sizeof(STD_KG_T), _Alignof(STD_KG_T));
	std_ptr_copy_nonoverlapping_Checked(scratch_ptr, v, len, sizeof(STD_KG_T), _Alignof(STD_KG_T));
}

/* The instance's mergesort_core sorts v by sorting each half and merging them, through the scratch buffer. */
static void
STD_KG_NAME(mergesort_core) (STD_KG_T * v, size_t len, STD_KG_T * scratch_ptr, StdSortLess *is_less)
{
	if (len > 2)
	{
		size_t		mid = STD_DIV_FOLDED(usize, len, 2);

		std_slice_get_unchecked_mut_range_check(0, mid, len, STD_KG_FILE, 82, 30);
		STD_KG_NAME(mergesort_core) (v, mid, scratch_ptr, is_less);
		std_slice_get_unchecked_mut_range_check(mid, len, len, STD_KG_FILE, 84, 30);
		STD_KG_NAME(mergesort_core) (&v[mid], std_wrapping_sub_usize(len, mid), scratch_ptr, is_less);
		STD_KG_NAME(merge) (v, len, scratch_ptr, is_less, mid);
	}
	else if (len == 2)
	{
		std_index_check(1, len, STD_KG_FILE, 89, 21);
		std_index_check(0, len, STD_KG_FILE, 89, 28);
		if (is_less->f(is_less->captures, &v[1], &v[0]))
			STD_KG_CAT(std_slice_swap_, STD_KG_TS) (v, len, 0, 1, STD_KG_FILE, 90, 15);
	}
}

/* The instance's mergesort_main frame is its state across its pad: its BufGuard, while live. */
typedef struct STD_KG_NAME (mergesort_main_frame_0_t)
{
	STD_KG_T   *v;
	size_t		len;
	StdSortLess *is_less;
	STD_KG_T   *buf_ptr;
	size_t		capacity;
	bool		buf_live;
} STD_KG_NAME(mergesort_main_frame_0_t);

/*
 * The instance's mergesort_main body is BufGuard::new's buffer of len
 * elements, "allocation failure" should the allocator return none, then
 * the sort.
 */
static void
STD_KG_NAME(mergesort_main_body_0) (void *frame_)
{
	STD_KG_NAME(mergesort_main_frame_0_t) * frame = frame_;

#ifdef USE_ASSERT_CHECKING
	if (!(frame->len > 0 && sizeof(STD_KG_T) > 0))
	{
		static const char text[] = "assertion failed: len > 0 && size_of::<T>() > 0";

		std_panic(STD_KG_FILE, 169, 9, text, sizeof(text) - 1);
	}
#endif
	frame->buf_ptr = std_global_alloc(std_wrapping_mul_usize(frame->len, sizeof(STD_KG_T)), _Alignof(STD_KG_T));
	if (frame->buf_ptr == NULL)
	{
		static const char text[] = "allocation failure";

		std_panic(STD_KG_FILE, 178, 13, text, sizeof(text) - 1);
	}
	frame->capacity = frame->len;
	frame->buf_live = true;
	STD_KG_NAME(mergesort_core) (frame->v, frame->len, frame->buf_ptr, frame->is_less);
	frame->buf_live = false;
	std_global_dealloc(frame->buf_ptr, std_wrapping_mul_usize(frame->capacity, sizeof(STD_KG_T)), _Alignof(STD_KG_T));
}

/* The instance's mergesort_main pad is BufGuard's drop: the buffer freed. */
static void
STD_KG_NAME(mergesort_main_pad_0) (void *frame_)
{
	STD_KG_NAME(mergesort_main_frame_0_t) * frame = frame_;

	if (frame->buf_live)
		std_global_dealloc(frame->buf_ptr, std_wrapping_mul_usize(frame->capacity, sizeof(STD_KG_T)), _Alignof(STD_KG_T));
}

/* The instance's sort closure is sort's is_less: a.lt(b), by the type's PartialOrd. */
static bool
STD_KG_NAME(sort_closure_0) (void *captures, const void *a, const void *b)
{
	return STD_KG_LT(a, b);
}

/* The instance's sort sorts v stably by the type's PartialOrd::lt, nothing for a zero-sized type or fewer than two elements. */
static void
STD_KG_NAME(sort) (STD_KG_T * v, size_t len)
{
	StdSortLess is_less = {STD_KG_NAME(sort_closure_0), NULL};

	STD_KG_NAME(mergesort_main_frame_0_t) frame =
	{
		.v = v, .len = len, .is_less = &is_less
	};

	if (sizeof(STD_KG_T) == 0)
		return;
	if (len < 2)
		return;
	std_landing(&frame, STD_KG_NAME(mergesort_main_body_0), STD_KG_NAME(mergesort_main_pad_0));
}

#undef STD_KG_CAT_
#undef STD_KG_CAT
#undef STD_KG_NAME
#undef STD_KG_FILE
#undef STD_KG_TS
#undef STD_KG_T
#undef STD_KG_LT
