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
 * The C port of core's sort tests' partial.rs (library/alloctests/tests/sort,
 * Rust 1.99.1): partial_sort_unstable's instances at i32 and (), the checks
 * at those types, and its tests basic_impl and random_patterns.
 */
#include "postgres.h"

#include "doltgres/std/alloc.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/num.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_sort.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#define RUST_FILE STD_RUST_LIBRARY STD_SEP "alloctests" STD_SEP "tests" STD_SEP "sort" STD_SEP "partial.rs"

/*
 * std_i32_PartialOrd_lt_FnMut_call_mut is the fn item i32's PartialOrd::lt's
 * FnMut shim, as the sort passes it for is_less.
 */
static bool
std_i32_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_i32_PartialOrd_lt(a, b);
}

/* The unstable sort and partial_sort_unstable at i32. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS I32
#define STD_SORT_T int32
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT std_i32_PartialOrd_lt_FnMut_call_mut
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_WITH_PARTIAL
#include "doltgres/std/sort_template.h"

/* partial_sort_unstable at (). */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS Unit
#define STD_SORT_UNIT
#define STD_SORT_WITH_PARTIAL
#include "doltgres/std/sort_template.h"

/* ALLOCTESTS_SORT_PARTIAL_UNBOUNDED is an unbounded end of a range, as .. has at both. */
#define ALLOCTESTS_SORT_PARTIAL_UNBOUNDED ((StdBound) {STD_BOUND_UNBOUNDED, 0})

/* ALLOCTESTS_SORT_PARTIAL_INCLUDED is a range's included start. */
#define ALLOCTESTS_SORT_PARTIAL_INCLUDED(i) ((StdBound) {STD_BOUND_INCLUDED, (i)})

/* ALLOCTESTS_SORT_PARTIAL_EXCLUDED is a range's excluded end. */
#define ALLOCTESTS_SORT_PARTIAL_EXCLUDED(i) ((StdBound) {STD_BOUND_EXCLUDED, (i)})

/*
 * alloctests_sort_partial_check_is_partial_sorted_I32 is
 * check_is_partial_sorted at i32: the range partially sorted, then the
 * last maximum before it, the range, and the first minimum after it in
 * order.
 */
static void
alloctests_sort_partial_check_is_partial_sorted_I32(int32 *v, size_t len, StdBound range_start, StdBound range_end)
{
	size_t		start;
	size_t		end;
	const int32 *max_before = NULL;
	const int32 *min_after = NULL;
	const int32 *last = NULL;
	bool		sorted = true;

	std_slice_range(range_start, range_end, len, &start, &end);
	std_slice_partial_sort_unstable_I32(v, len, ALLOCTESTS_SORT_PARTIAL_INCLUDED(start),
										ALLOCTESTS_SORT_PARTIAL_EXCLUDED(end));
	std_range_to_check(start, len, RUST_FILE, 11, 23);
	for (size_t k = 0; k < start; k++)
		if (max_before == NULL || std_i32_Ord_cmp(max_before, &v[k]) != STD_ORDERING_GREATER)
			max_before = &v[k];
	std_range_check(start, end, len, RUST_FILE, 12, 25);
	std_range_from_check(end, len, RUST_FILE, 13, 22);
	for (size_t k = end; k < len; k++)
		if (min_after == NULL || std_i32_Ord_cmp(min_after, &v[k]) == STD_ORDERING_GREATER)
			min_after = &v[k];
	if (max_before != NULL)
		last = max_before;
	for (size_t k = start; sorted && k < end; k++)
	{
		if (last != NULL && !(*last <= v[k]))
			sorted = false;
		last = &v[k];
	}
	if (sorted && min_after != NULL && last != NULL && !(*last <= *min_after))
		sorted = false;
	STD_ASSERT(15, 5, sorted, "seq.is_sorted()");
}

/*
 * alloctests_sort_partial_check_is_partial_sorted_Unit is
 * check_is_partial_sorted at (): the range and its slices checked, with
 * every sequence of () sorted.
 */
static void
alloctests_sort_partial_check_is_partial_sorted_Unit(size_t len, StdBound range_start, StdBound range_end)
{
	size_t		start;
	size_t		end;

	std_slice_range(range_start, range_end, len, &start, &end);
	std_slice_partial_sort_unstable_Unit(len, ALLOCTESTS_SORT_PARTIAL_INCLUDED(start),
										 ALLOCTESTS_SORT_PARTIAL_EXCLUDED(end));
	std_range_to_check(start, len, RUST_FILE, 11, 23);
	std_range_check(start, end, len, RUST_FILE, 12, 25);
	std_range_from_check(end, len, RUST_FILE, 13, 22);
}

/* test_alloctests_sort_partial_basic_impl is the test basic_impl: short slices of () and of i32 at several ranges. */
void
test_alloctests_sort_partial_basic_impl(void)
{
	alloctests_sort_partial_check_is_partial_sorted_I32(STD_DANGLING_FOR(_Alignof(int32)), 0,
														ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
														ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	alloctests_sort_partial_check_is_partial_sorted_Unit(0, ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
														 ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	alloctests_sort_partial_check_is_partial_sorted_Unit(1, ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
														 ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	alloctests_sort_partial_check_is_partial_sorted_Unit(2, ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
														 ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	alloctests_sort_partial_check_is_partial_sorted_Unit(3, ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
														 ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	alloctests_sort_partial_check_is_partial_sorted_I32(STD_DANGLING_FOR(_Alignof(int32)), 0,
														ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
														ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	{
		int32		v[] = {77};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
															ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	}
	{
		int32		v[] = {2, 3};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
															ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	}
	{
		int32		v[] = {2, 3, 6};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
															ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	}
	{
		int32		v[] = {2, 3, 99, 6};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
															ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	}
	{
		int32		v[] = {2, 7709, 400, 90932};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
															ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_UNBOUNDED,
															ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(0),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(0));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(0),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(1));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(0),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(5));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(0),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(7));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(7),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(7));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(6),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(7));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(5),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(7));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(5),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(5));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(4),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(5));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_partial_check_is_partial_sorted_I32(v, lengthof(v), ALLOCTESTS_SORT_PARTIAL_INCLUDED(4),
															ALLOCTESTS_SORT_PARTIAL_EXCLUDED(6));
	}
}

/* alloctests_sort_partial_check_is_partial_sorted_ranges_frame_0_t is check_is_partial_sorted_ranges' state across its pad: the temporary copy a statement checks, while live. */
typedef struct alloctests_sort_partial_check_is_partial_sorted_ranges_frame_0_t
{
	const int32 *v;
	size_t		len;
	StdVec		tmp;
	bool		tmp_live;
} alloctests_sort_partial_check_is_partial_sorted_ranges_frame_0_t;

/* alloctests_sort_partial_check_on_copy is one of check_is_partial_sorted_ranges' statements: the check on v.to_vec(), the temporary dropped after. */
static void
alloctests_sort_partial_check_on_copy(alloctests_sort_partial_check_is_partial_sorted_ranges_frame_0_t *frame, StdBound start,
									  StdBound end)
{
	std_slice_to_vec_Checked(&frame->tmp, frame->v, frame->len, sizeof(int32), _Alignof(int32));
	frame->tmp_live = true;
	alloctests_sort_partial_check_is_partial_sorted_I32(frame->tmp.ptr, frame->tmp.len, start, end);
	frame->tmp_live = false;
	std_vec_free_Checked(&frame->tmp, sizeof(int32), _Alignof(int32));
}

/* alloctests_sort_partial_check_is_partial_sorted_ranges_body_0 is check_is_partial_sorted_ranges at i32: copies of v checked at the ends, around each midpoint, and between its quarters. */
static void
alloctests_sort_partial_check_is_partial_sorted_ranges_body_0(void *frame_)
{
	alloctests_sort_partial_check_is_partial_sorted_ranges_frame_0_t *frame = frame_;
	size_t		len = frame->len;

	alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_UNBOUNDED, ALLOCTESTS_SORT_PARTIAL_UNBOUNDED);
	alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(0),
										  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(0));
	alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(len),
										  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(len));
	if (len > 0)
	{
		size_t		quarters[5];

		{
			size_t		start = STD_SUB(usize, len, 1, 26, 58);
			size_t		end = STD_SUB(usize, len, 1, 26, 67);

			alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(start),
												  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(end));
		}
		alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(0),
											  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(1));
		alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(STD_SUB(usize, len, 1, 28, 58)),
											  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(len));
		for (size_t mid = 1; mid < len; mid = std_forward_unchecked_usize_Checked(mid, 1))
		{
			alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(0),
												  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(mid));
			alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(mid),
												  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(len));
			alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(mid),
												  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(mid));
			{
				size_t		start = STD_SUB(usize, mid, 1, 34, 62);
				size_t		end = STD_ADD(usize, mid, 1, 34, 71);

				alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(start),
													  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(end));
			}
			alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(STD_SUB(usize, mid, 1, 35, 62)),
												  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(mid));
			alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(mid),
												  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(STD_ADD(usize, mid, 1, 36, 67)));
		}
		quarters[0] = 0;
		quarters[1] = STD_DIV_FOLDED(usize, len, 4);
		quarters[2] = STD_DIV_FOLDED(usize, len, 2);
		quarters[3] = STD_DIV_FOLDED(usize, STD_MUL(usize, 3, len, 39, 47), 4);
		quarters[4] = len;
		for (size_t s = 0; s < lengthof(quarters); s++)
			for (size_t e = 0; e < lengthof(quarters); e++)
				if (quarters[s] < quarters[e])
					alloctests_sort_partial_check_on_copy(frame, ALLOCTESTS_SORT_PARTIAL_INCLUDED(quarters[s]),
														  ALLOCTESTS_SORT_PARTIAL_EXCLUDED(quarters[e]));
	}
}

/* alloctests_sort_partial_check_is_partial_sorted_ranges_pad_0 is its pad: the temporary copy, while live. */
static void
alloctests_sort_partial_check_is_partial_sorted_ranges_pad_0(void *frame_)
{
	alloctests_sort_partial_check_is_partial_sorted_ranges_frame_0_t *frame = frame_;

	if (frame->tmp_live)
		std_vec_free_Checked(&frame->tmp, sizeof(int32), _Alignof(int32));
}

/* alloctests_sort_partial_check_is_partial_sorted_ranges_I32 is check_is_partial_sorted_ranges at i32. */
static void
alloctests_sort_partial_check_is_partial_sorted_ranges_I32(const int32 *v, size_t len)
{
	alloctests_sort_partial_check_is_partial_sorted_ranges_frame_0_t frame = {.v = v, .len = len};

	std_landing(&frame, alloctests_sort_partial_check_is_partial_sorted_ranges_body_0,
				alloctests_sort_partial_check_is_partial_sorted_ranges_pad_0);
}

/* alloctests_sort_partial_random_patterns_frame_0_t is random_patterns' state across its pad: the pattern a statement checks, while live. */
typedef struct alloctests_sort_partial_random_patterns_frame_0_t
{
	StdVec		pattern;
	bool		pattern_live;
} alloctests_sort_partial_random_patterns_frame_0_t;

/* alloctests_sort_partial_random_patterns_check is one of random_patterns' statements: the range checks on a random pattern of a length, the temporary dropped after. */
static void
alloctests_sort_partial_random_patterns_check(alloctests_sort_partial_random_patterns_frame_0_t *frame, size_t len)
{
	alloctests_sort_patterns_random(len, &frame->pattern);
	frame->pattern_live = true;
	alloctests_sort_partial_check_is_partial_sorted_ranges_I32(frame->pattern.ptr, frame->pattern.len);
	frame->pattern_live = false;
	std_vec_free_Checked(&frame->pattern, sizeof(int32), _Alignof(int32));
}

/* alloctests_sort_partial_random_patterns_body_0 is random_patterns: the range checks on random patterns of 10 and 50, and outside Miri of 100 and 1000. */
static void
alloctests_sort_partial_random_patterns_body_0(void *frame_)
{
	alloctests_sort_partial_random_patterns_frame_0_t *frame = frame_;

	alloctests_sort_partial_random_patterns_check(frame, 10);
	alloctests_sort_partial_random_patterns_check(frame, 50);
	alloctests_sort_partial_random_patterns_check(frame, 100);
	alloctests_sort_partial_random_patterns_check(frame, 1000);
}

/* alloctests_sort_partial_random_patterns_pad_0 is random_patterns' pad: the pattern, while live. */
static void
alloctests_sort_partial_random_patterns_pad_0(void *frame_)
{
	alloctests_sort_partial_random_patterns_frame_0_t *frame = frame_;

	if (frame->pattern_live)
		std_vec_free_Checked(&frame->pattern, sizeof(int32), _Alignof(int32));
}

/* test_alloctests_sort_partial_random_patterns is the test random_patterns. */
void
test_alloctests_sort_partial_random_patterns(void)
{
	alloctests_sort_partial_random_patterns_frame_0_t frame = {0};

	std_landing(&frame, alloctests_sort_partial_random_patterns_body_0, alloctests_sort_partial_random_patterns_pad_0);
}
