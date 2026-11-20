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
 * The template of slices' sort_unstable (the port text's sections 2 and 9):
 * a port of core/src/slice/sort/shared and unstable at Rust 1.99.1, in
 * their modules' order, at an element type T: ipnsort with its run
 * detection, its quicksort with the ancestor pivot, its partitions, its
 * pivot choice, its heapsort fallback, and the small sorts T's size,
 * Freeze, and Copy pick, so the comparator runs on the same pairs in the
 * same order as the Rust's, with temporaries and scratch arrays of T as
 * Rust's are. The guards that write elements back when the comparator
 * panics are landing pads. A crate's sort is codegened in the crate, so its
 * pointer copies run their precondition checks (std/ptr.h). Rust's std is
 * built without overflow checks, so its arithmetic here is plain C's, and
 * its intrinsics::abort is abort.
 *
 * STD_SORT_SCOPE is static or extern, STD_SORT_DECLARE and STD_SORT_DEFINE
 * choose the declarations and definitions, STD_SORT_TS spells T, STD_SORT_T
 * is T, STD_SORT_FREEZE and STD_SORT_COPY are 1 when T is Freeze and Copy,
 * STD_SORT_BRANCHLESS is 1 when T is at most 96 bytes, inst_partition's
 * choice of the branchless cyclic partition over Hoare's, which a static
 * assertion holds to T's size, and STD_SORT_LT is T's PartialOrd::lt's FnMut
 * shim, a StdIsLess, which sort_unstable passes; STD_SORT_WITH_BY, when
 * defined, adds sort_unstable_by's instance, and STD_SORT_WITHOUT_UNSTABLE
 * leaves sort_unstable's out for an instance that sorts only by a comparison,
 * which then needs no STD_SORT_LT. STD_SORT_WITH_PARTIAL, when defined, adds
 * <[T]>::partial_sort_unstable's instance with select.rs' partition_at_index,
 * ported from core/src/slice/sort/select.rs; with STD_SORT_UNIT also
 * defined, T is (), and that instance, which returns at once, is all the
 * inclusion defines. The defining inclusion also defines ptr::swap's and slice::swap's
 * instances at T. STD_SORT_SHARED_ONLY, when defined, leaves out everything
 * but core's shared parts (find_existing_run, choose_pivot, the insertion
 * sort, and the stable small sort with its merges), which
 * stable_sort_template.h's instance at the same T calls, and then needs no
 * STD_SORT_LT, STD_SORT_COPY, or STD_SORT_BRANCHLESS. Every parameter is
 * undefined at the end.
 */

/* STD_SORT_CAT pastes two tokens after expanding them. */
#define STD_SORT_CAT_(a, b) a##b
#define STD_SORT_CAT(a, b) STD_SORT_CAT_(a, b)

/* STD_SORT_DEF is STD_SORT_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_SORT_DEF_static static
#define STD_SORT_DEF_extern
#define STD_SORT_DEF STD_SORT_CAT(STD_SORT_DEF_, STD_SORT_SCOPE)

/* STD_SORT_NAME names the sort's functions at T. */
#define STD_SORT_NAME(name) STD_SORT_CAT(std_sort_##name##_, STD_SORT_TS)

/* STD_SORT_UNSTABLE is <[T]>::sort_unstable's instance. */
#define STD_SORT_UNSTABLE STD_SORT_CAT(std_slice_sort_unstable_, STD_SORT_TS)

/* STD_SORT_UNSTABLE_BY is <[T]>::sort_unstable_by's instance. */
#define STD_SORT_UNSTABLE_BY STD_SORT_CAT(std_slice_sort_unstable_by_, STD_SORT_TS)

#ifdef STD_SORT_UNIT
#ifdef STD_SORT_DECLARE
STD_SORT_SCOPE void STD_SORT_CAT(std_slice_partial_sort_unstable_, STD_SORT_TS) (size_t len, StdBound start,
																				 StdBound end);
#endif
#ifdef STD_SORT_DEFINE
/* partial_sort_unstable at (): partial_sort returns at once for a zero-sized T, before it reads the range. */
STD_SORT_DEF void
STD_SORT_CAT(std_slice_partial_sort_unstable_, STD_SORT_TS) (size_t len, StdBound start, StdBound end)
{
}
#endif
#else

#ifdef STD_SORT_DECLARE
#ifndef STD_SORT_WITHOUT_UNSTABLE
STD_SORT_SCOPE void STD_SORT_UNSTABLE(STD_SORT_T * v, size_t len);
#endif
#ifdef STD_SORT_WITH_BY
STD_SORT_SCOPE void STD_SORT_UNSTABLE_BY(STD_SORT_T * v, size_t len,
										 StdOrdering (*compare) (void *, const void *, const void *),
										 void *compare_captures, void (*compare_drop) (void *));
#endif
#ifndef STD_SORT_SHARED_ONLY
STD_SORT_SCOPE void STD_SORT_NAME(sort) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
#endif							/* STD_SORT_SHARED_ONLY */
#if defined(STD_SORT_WITH_PARTIAL) && !defined(STD_SORT_SHARED_ONLY)
STD_SORT_SCOPE void STD_SORT_CAT(std_slice_partial_sort_unstable_, STD_SORT_TS) (STD_SORT_T * v, size_t len,
																				 StdBound start, StdBound end);
#endif
#endif

#ifdef STD_SORT_DEFINE

#ifndef STD_SORT_SHARED_ONLY
/* ptr::swap and slice::swap at T, which the sort's swaps reach. */
#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS STD_SORT_TS
#define STD_PTR_T STD_SORT_T
#include "doltgres/std/ptr_template.h"
#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS STD_SORT_TS
#define STD_SLICE_T STD_SORT_T
#include "doltgres/std/slice_template.h"
#endif							/* STD_SORT_SHARED_ONLY */

/* STD_SORT_SIZE and STD_SORT_ALIGN are T's size and alignment, which its pointer copies take. */
#define STD_SORT_SIZE sizeof(STD_SORT_T)
#define STD_SORT_ALIGN _Alignof(STD_SORT_T)

#ifndef STD_SORT_SHARED_ONLY
StaticAssertDecl((sizeof(STD_SORT_T) <= STD_SORT_MAX_BRANCHLESS_PARTITION_SIZE) == STD_SORT_BRANCHLESS,
				 "STD_SORT_BRANCHLESS is inst_partition's choice for T's size");
#endif							/* STD_SORT_SHARED_ONLY */

#ifndef STD_SORT_SHARED_ONLY
/* STD_SORT_SWAP and STD_SORT_SWAP_UNCHECKED are slice::swap's and swap_unchecked's instances at T. */
#define STD_SORT_SWAP STD_SORT_CAT(std_slice_swap_, STD_SORT_TS)
#define STD_SORT_SWAP_UNCHECKED STD_SORT_CAT(std_slice_swap_unchecked_, STD_SORT_TS)
#endif							/* STD_SORT_SHARED_ONLY */

static size_t STD_SORT_NAME(find_existing_run) (const STD_SORT_T * v, size_t len, StdSortLess *is_less,
												bool *result_1);
static size_t STD_SORT_NAME(choose_pivot) (const STD_SORT_T * v, size_t len, StdSortLess *is_less);
static const STD_SORT_T *STD_SORT_NAME(median3_rec) (const STD_SORT_T * a, const STD_SORT_T * b,
													 const STD_SORT_T * c, size_t n, StdSortLess *is_less);
static const STD_SORT_T *STD_SORT_NAME(median3) (const STD_SORT_T * a, const STD_SORT_T * b,
												 const STD_SORT_T * c, StdSortLess *is_less);
#ifndef STD_SORT_SHARED_ONLY
static size_t STD_SORT_NAME(small_sort_threshold) (void);
static void STD_SORT_NAME(small_sort) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
static void STD_SORT_NAME(small_sort_fallback) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
#if STD_SORT_FREEZE
static void STD_SORT_NAME(small_sort_general) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
#endif
#endif							/* STD_SORT_SHARED_ONLY */
#if STD_SORT_FREEZE
static void STD_SORT_NAME(small_sort_general_with_scratch) (STD_SORT_T * v, size_t len, STD_SORT_T * scratch,
															size_t scratch_len, StdSortLess *is_less);
#endif
static void STD_SORT_NAME(CopyOnDrop_drop) (const STD_SORT_T * src, STD_SORT_T * dst, size_t len);
#ifndef STD_SORT_SHARED_ONLY
#if STD_SORT_FREEZE && STD_SORT_COPY
static void STD_SORT_NAME(small_sort_network) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
#endif
#if STD_SORT_FREEZE && STD_SORT_COPY
static void STD_SORT_NAME(swap_if_less) (STD_SORT_T * v_base, size_t a_pos, size_t b_pos, StdSortLess *is_less);
#endif
#if STD_SORT_FREEZE && STD_SORT_COPY
static void STD_SORT_NAME(sort9_optimal) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
#endif
#if STD_SORT_FREEZE && STD_SORT_COPY
static void STD_SORT_NAME(sort13_optimal) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
#endif
#endif							/* STD_SORT_SHARED_ONLY */
static void STD_SORT_NAME(insert_tail) (STD_SORT_T * begin, STD_SORT_T * tail, StdSortLess *is_less);
static void STD_SORT_NAME(insertion_sort_shift_left) (STD_SORT_T * v, size_t len, size_t offset,
													  StdSortLess *is_less);
#if STD_SORT_FREEZE
static void STD_SORT_NAME(sort4_stable) (const STD_SORT_T * v_base, STD_SORT_T * dst, StdSortLess *is_less);
#endif
#if STD_SORT_FREEZE
static void STD_SORT_NAME(sort8_stable) (STD_SORT_T * v_base, STD_SORT_T * dst, STD_SORT_T * scratch_base,
										 StdSortLess *is_less);
#endif
#if STD_SORT_FREEZE
static void STD_SORT_NAME(merge_up) (const STD_SORT_T * left_src, const STD_SORT_T * right_src, STD_SORT_T * dst,
									 StdSortLess *is_less, const STD_SORT_T * *result_0,
									 const STD_SORT_T * *result_1, STD_SORT_T * *result_2);
#endif
#if STD_SORT_FREEZE
static void STD_SORT_NAME(merge_down) (const STD_SORT_T * left_src, const STD_SORT_T * right_src,
									   STD_SORT_T * dst, StdSortLess *is_less, const STD_SORT_T * *result_0,
									   const STD_SORT_T * *result_1, STD_SORT_T * *result_2);
#endif
#if STD_SORT_FREEZE
static void STD_SORT_NAME(bidirectional_merge) (const STD_SORT_T * v, size_t len, STD_SORT_T * dst,
												StdSortLess *is_less);
#endif
#ifndef STD_SORT_SHARED_ONLY
static void STD_SORT_NAME(ipnsort) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
static void STD_SORT_NAME(heapsort) (STD_SORT_T * v, size_t len, StdSortLess *is_less);
static void STD_SORT_NAME(sift_down) (STD_SORT_T * v, size_t len, size_t node, StdSortLess *is_less);
static void STD_SORT_NAME(quicksort) (STD_SORT_T * v, size_t len, const STD_SORT_T * ancestor_pivot, uint32 limit,
									  StdSortLess *is_less);
static size_t STD_SORT_NAME(partition) (STD_SORT_T * v, size_t len, size_t pivot, StdSortLess *is_less);
#if !STD_SORT_BRANCHLESS
static size_t STD_SORT_NAME(partition_hoare_branchy_cyclic) (STD_SORT_T * v, size_t len, const STD_SORT_T * pivot,
															 StdSortLess *is_less);
#else
static size_t STD_SORT_NAME(partition_lomuto_branchless_cyclic) (STD_SORT_T * v, size_t len,
																 const STD_SORT_T * pivot, StdSortLess *is_less);
#endif
#endif							/* STD_SORT_SHARED_ONLY */

/*
 * find_existing_run returns the length of the sorted run at a slice's
 * start, and whether it strictly descends.
 */
static size_t
STD_SORT_NAME(find_existing_run) (const STD_SORT_T * v, size_t len, StdSortLess *is_less, bool *result_1)
{
	size_t		run_len;
	bool		strictly_descending;

	if (len < 2)
	{
		*result_1 = false;
		return len;
	}
	run_len = 2;
	std_slice_get_unchecked_check(1, len, STD_SORT_SHARED_FILE, 34, 45);
	std_slice_get_unchecked_check(0, len, STD_SORT_SHARED_FILE, 34, 65);
	strictly_descending = STD_SORT_LESS(is_less, v + 1, v);
	if (strictly_descending)
	{
		for (;;)
		{
			if (!(run_len < len))
				break;
			std_slice_get_unchecked_check(run_len, len, STD_SORT_SHARED_FILE, 36, 46);
			std_slice_get_unchecked_check(run_len - 1, len, STD_SORT_SHARED_FILE, 36, 72);
			if (!STD_SORT_LESS(is_less, v + run_len, v + (run_len - 1)))
				break;
			run_len += 1;
		}
	}
	else
	{
		for (;;)
		{
			if (!(run_len < len))
				break;
			std_slice_get_unchecked_check(run_len, len, STD_SORT_SHARED_FILE, 40, 47);
			std_slice_get_unchecked_check(run_len - 1, len, STD_SORT_SHARED_FILE, 40, 73);
			if (STD_SORT_LESS(is_less, v + run_len, v + (run_len - 1)))
				break;
			run_len += 1;
		}
	}
	*result_1 = strictly_descending;
	return run_len;
}

/*
 * choose_pivot returns the index of a pivot sampled adaptively from a slice
 * of eight or more elements.
 */
static size_t
STD_SORT_NAME(choose_pivot) (const STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	size_t		index;

	if (len < 8)
		std_intrinsics_abort();
	{
		const		STD_SORT_T *v_base = v;
		size_t		len_div_8 = len / 8;
		const		STD_SORT_T *a = v_base;
		const		STD_SORT_T *b = v_base + len_div_8 * 4;
		const		STD_SORT_T *c = v_base + len_div_8 * 7;

		if (len < STD_SORT_PSEUDO_MEDIAN_REC_THRESHOLD)
			index = std_ptr_offset_from_unsigned_Checked(STD_SORT_NAME(median3) (a, b, c, is_less), v_base, STD_SORT_SIZE,
														 STD_SORT_PIVOT_FILE, 35, 45);
		else
			index = std_ptr_offset_from_unsigned_Checked(STD_SORT_NAME(median3_rec) (a, b, c, len_div_8, is_less), v_base,
														 STD_SORT_SIZE, STD_SORT_PIVOT_FILE, 37, 54);
	}
	std_hint_assert_unchecked(index < len, STD_SORT_PIVOT_FILE, 42, 9);
	return index;
}

/*
 * median3_rec returns a pseudomedian of three regions of n elements,
 * recursing into large regions.
 */
static const STD_SORT_T *
STD_SORT_NAME(median3_rec) (const STD_SORT_T * a, const STD_SORT_T * b, const STD_SORT_T * c, size_t n,
							StdSortLess *is_less)
{
	if (n * 8 >= STD_SORT_PSEUDO_MEDIAN_REC_THRESHOLD)
	{
		size_t		n8 = n / 8;

		a = STD_SORT_NAME(median3_rec) (a, a + n8 * 4, a + n8 * 7, n8, is_less);
		b = STD_SORT_NAME(median3_rec) (b, b + n8 * 4, b + n8 * 7, n8, is_less);
		c = STD_SORT_NAME(median3_rec) (c, c + n8 * 4, c + n8 * 7, n8, is_less);
	}
	return STD_SORT_NAME(median3) (a, b, c, is_less);
}

/* median3 returns the median of three elements. */
static const STD_SORT_T *
STD_SORT_NAME(median3) (const STD_SORT_T * a, const STD_SORT_T * b, const STD_SORT_T * c, StdSortLess *is_less)
{
	bool		x = STD_SORT_LESS(is_less, a, b);
	bool		y = STD_SORT_LESS(is_less, a, c);

	if (x == y)
	{
		bool		z = STD_SORT_LESS(is_less, b, c);

		return (z ^ x) ? c : b;
	}
	else
		return a;
}

#ifndef STD_SORT_SHARED_ONLY
/*
 * small_sort_threshold is UnstableSmallSortTypeImpl's threshold: the
 * network's for a Freeze and Copy element with an efficient in-place swap
 * whose scratch fits the stack, the general one's for another Freeze element
 * whose scratch fits, and the fallback's otherwise.
 */
static size_t
STD_SORT_NAME(small_sort_threshold) (void)
{
#if STD_SORT_FREEZE && STD_SORT_COPY
	if (STD_SORT_SIZE <= 8 && STD_SORT_SIZE * STD_SORT_SMALL_SORT_NETWORK_SCRATCH_LEN <= STD_SORT_MAX_STACK_ARRAY_SIZE)
		return STD_SORT_SMALL_SORT_NETWORK_THRESHOLD;
	if (STD_SORT_SIZE * STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN <= STD_SORT_MAX_STACK_ARRAY_SIZE)
		return STD_SORT_SMALL_SORT_GENERAL_THRESHOLD;
	return STD_SORT_SMALL_SORT_FALLBACK_THRESHOLD;
#elif STD_SORT_FREEZE
	if (STD_SORT_SIZE * STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN <= STD_SORT_MAX_STACK_ARRAY_SIZE)
		return STD_SORT_SMALL_SORT_GENERAL_THRESHOLD;
	return STD_SORT_SMALL_SORT_FALLBACK_THRESHOLD;
#else
	return STD_SORT_SMALL_SORT_FALLBACK_THRESHOLD;
#endif
}

/* small_sort is UnstableSmallSortTypeImpl's small_sort, by the same choice as its threshold. */
static void
STD_SORT_NAME(small_sort) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
#if STD_SORT_FREEZE && STD_SORT_COPY
	if (STD_SORT_SIZE <= 8 && STD_SORT_SIZE * STD_SORT_SMALL_SORT_NETWORK_SCRATCH_LEN <= STD_SORT_MAX_STACK_ARRAY_SIZE)
		STD_SORT_NAME(small_sort_network) (v, len, is_less);
	else if (STD_SORT_SIZE * STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN <= STD_SORT_MAX_STACK_ARRAY_SIZE)
		STD_SORT_NAME(small_sort_general) (v, len, is_less);
	else
		STD_SORT_NAME(small_sort_fallback) (v, len, is_less);
#elif STD_SORT_FREEZE
	if (STD_SORT_SIZE * STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN <= STD_SORT_MAX_STACK_ARRAY_SIZE)
		STD_SORT_NAME(small_sort_general) (v, len, is_less);
	else
		STD_SORT_NAME(small_sort_fallback) (v, len, is_less);
#else
	STD_SORT_NAME(small_sort_fallback) (v, len, is_less);
#endif
}

/* small_sort_fallback sorts a short slice by insertion. */
static void
STD_SORT_NAME(small_sort_fallback) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	if (len >= 2)
		STD_SORT_NAME(insertion_sort_shift_left) (v, len, 1, is_less);
}
#endif							/* STD_SORT_SHARED_ONLY */

#if STD_SORT_FREEZE

#ifndef STD_SORT_SHARED_ONLY
/* small_sort_general sorts a short slice with a scratch array of T on the stack. */
static void
STD_SORT_NAME(small_sort_general) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	STD_SORT_T	stack_array[STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN];

	std_slice_from_raw_parts_mut_check(stack_array, STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN, STD_SORT_SIZE,
									   STD_SORT_ALIGN, STD_SORT_SMALLSORT_FILE, 211, 9);
	STD_SORT_NAME(small_sort_general_with_scratch) (v, len, stack_array, STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN,
													is_less);
}

#endif							/* STD_SORT_SHARED_ONLY */
/* STD_SORT_SMALL_SORT_GENERAL_WITH_SCRATCH_FRAME_0_T is the merge's state across its pad: its CopyOnDrop guard and is_less. */
#define STD_SORT_SMALL_SORT_GENERAL_WITH_SCRATCH_FRAME_0_T STD_SORT_NAME(small_sort_general_with_scratch_frame_0_t)
typedef struct STD_SORT_SMALL_SORT_GENERAL_WITH_SCRATCH_FRAME_0_T
{
	STD_SORT_T *drop_guard_src;
	STD_SORT_T *drop_guard_dst;
	size_t		drop_guard_len;
	StdSortLess *is_less;
}			STD_SORT_SMALL_SORT_GENERAL_WITH_SCRATCH_FRAME_0_T;

/* small_sort_general_with_scratch_body_0 is the merge of the presorted halves in scratch back into the slice. */
static void
STD_SORT_NAME(small_sort_general_with_scratch_body_0) (void *frame_)
{
	STD_SORT_SMALL_SORT_GENERAL_WITH_SCRATCH_FRAME_0_T *frame = frame_;

	std_slice_from_raw_parts_check(frame->drop_guard_src, frame->drop_guard_len, STD_SORT_SIZE, STD_SORT_ALIGN,
								   STD_SORT_SMALLSORT_FILE, 287, 13);
	STD_SORT_NAME(bidirectional_merge) (frame->drop_guard_src, frame->drop_guard_len, frame->drop_guard_dst,
										frame->is_less);
}

/* small_sort_general_with_scratch_pad_0 is the merge's pad: CopyOnDrop's drop, scratch copied back over the slice. */
static void
STD_SORT_NAME(small_sort_general_with_scratch_pad_0) (void *frame_)
{
	STD_SORT_SMALL_SORT_GENERAL_WITH_SCRATCH_FRAME_0_T *frame = frame_;

	STD_SORT_NAME(CopyOnDrop_drop) (frame->drop_guard_src, frame->drop_guard_dst, frame->drop_guard_len);
}

/*
 * small_sort_general_with_scratch sorts a short slice by presorting each
 * half into scratch and merging them back, the guard copying scratch back
 * should the merge panic.
 */
static void
STD_SORT_NAME(small_sort_general_with_scratch) (STD_SORT_T * v, size_t len, STD_SORT_T * scratch,
												size_t scratch_len, StdSortLess *is_less)
{
	STD_SORT_T *v_base;
	size_t		len_div_2;
	STD_SORT_T *scratch_base;
	size_t		presorted_len;
	STD_SORT_SMALL_SORT_GENERAL_WITH_SCRATCH_FRAME_0_T frame;

	if (len < 2)
		return;
	if (scratch_len < len + 16)
		std_intrinsics_abort();
	v_base = v;
	len_div_2 = len / 2;
	scratch_base = scratch;
	if (STD_SORT_SIZE <= 16 && len >= 16)
	{
		STD_SORT_NAME(sort8_stable) (v_base, scratch_base, scratch_base + len, is_less);
		STD_SORT_NAME(sort8_stable) (v_base + len_div_2, scratch_base + len_div_2, scratch_base + (len + 8), is_less);
		presorted_len = 8;
	}
	else if (len >= 8)
	{
		STD_SORT_NAME(sort4_stable) (v_base, scratch_base, is_less);
		STD_SORT_NAME(sort4_stable) (v_base + len_div_2, scratch_base + len_div_2, is_less);
		presorted_len = 4;
	}
	else
	{
		std_ptr_copy_nonoverlapping_Checked(v_base, scratch_base, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
		std_ptr_copy_nonoverlapping_Checked(v_base + len_div_2, scratch_base + len_div_2, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
		presorted_len = 1;
	}
	for (int k = 0; k < 2; k++)
	{
		size_t		offset = k == 0 ? 0 : len_div_2;
		STD_SORT_T *src = v_base + offset;
		STD_SORT_T *dst = scratch_base + offset;
		size_t		desired_len = offset == 0 ? len_div_2 : len - len_div_2;

		for (size_t i = presorted_len, i_end = desired_len; i < i_end; i++)
		{
			std_ptr_copy_nonoverlapping_Checked(src + i, dst + i, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
			STD_SORT_NAME(insert_tail) (dst, dst + i, is_less);
		}
	}
	frame.drop_guard_src = scratch_base;
	frame.drop_guard_dst = v_base;
	frame.drop_guard_len = len;
	frame.is_less = is_less;
	std_landing(&frame, STD_SORT_NAME(small_sort_general_with_scratch_body_0),
				STD_SORT_NAME(small_sort_general_with_scratch_pad_0));
}

#endif

/* CopyOnDrop_drop is CopyOnDrop's drop: len elements copied from src to dst, the write-back of a guard. */
static void
STD_SORT_NAME(CopyOnDrop_drop) (const STD_SORT_T * src, STD_SORT_T * dst, size_t len)
{
	std_ptr_copy_nonoverlapping_Checked(src, dst, len, STD_SORT_SIZE, STD_SORT_ALIGN);
}

#ifndef STD_SORT_SHARED_ONLY
#if STD_SORT_FREEZE && STD_SORT_COPY

/*
 * small_sort_network sorts a short slice of small elements by sorting
 * networks, insertion, and a merge through a scratch array of T.
 */
static void
STD_SORT_NAME(small_sort_network) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	STD_SORT_T	stack_array[STD_SORT_SMALL_SORT_NETWORK_SCRATCH_LEN];
	size_t		len_div_2;
	bool		no_merge;
	STD_SORT_T *v_base;
	size_t		initial_region_len;
	STD_SORT_T *region;
	size_t		region_len;

	if (len < 2)
		return;
	if (len > STD_SORT_SMALL_SORT_NETWORK_SCRATCH_LEN)
		std_intrinsics_abort();
	len_div_2 = len / 2;
	no_merge = len < 18;
	v_base = v;
	initial_region_len = no_merge ? len : len_div_2;
	std_slice_from_raw_parts_mut_check(v_base, initial_region_len, STD_SORT_SIZE, STD_SORT_ALIGN,
									   STD_SORT_SMALLSORT_FILE, 335, 31);
	region = v_base;
	region_len = initial_region_len;
	for (;;)
	{
		size_t		presorted_len;

		if (region_len >= 13)
		{
			STD_SORT_NAME(sort13_optimal) (region, region_len, is_less);
			presorted_len = 13;
		}
		else if (region_len >= 9)
		{
			STD_SORT_NAME(sort9_optimal) (region, region_len, is_less);
			presorted_len = 9;
		}
		else
			presorted_len = 1;
		STD_SORT_NAME(insertion_sort_shift_left) (region, region_len, presorted_len, is_less);
		if (no_merge)
			return;
		if (region != v_base)
			break;
		std_slice_from_raw_parts_mut_check(v_base + len_div_2, len - len_div_2, STD_SORT_SIZE, STD_SORT_ALIGN,
										   STD_SORT_SMALLSORT_FILE, 360, 27);
		region = v_base + len_div_2;
		region_len = len - len_div_2;
	}
	{
		STD_SORT_T *scratch_base = stack_array;

		std_slice_from_raw_parts_mut_check(v_base, len, STD_SORT_SIZE, STD_SORT_ALIGN, STD_SORT_SMALLSORT_FILE, 368, 29);
		STD_SORT_NAME(bidirectional_merge) (v_base, len, scratch_base, is_less);
		std_ptr_copy_nonoverlapping_Checked(scratch_base, v_base, len, STD_SORT_SIZE, STD_SORT_ALIGN);
	}
}

/* swap_if_less swaps two elements when the second orders before the first, through a temporary of T. */
static void
STD_SORT_NAME(swap_if_less) (STD_SORT_T * v_base, size_t a_pos, size_t b_pos, StdSortLess *is_less)
{
	STD_SORT_T *v_a = v_base + a_pos;
	STD_SORT_T *v_b = v_base + b_pos;
	bool		should_swap = STD_SORT_LESS(is_less, v_b, v_a);
	STD_SORT_T *v_a_swap = should_swap ? v_b : v_a;
	STD_SORT_T *v_b_swap = should_swap ? v_a : v_b;
	STD_SORT_T	v_b_swap_tmp = *v_b_swap;

	std_ptr_copy(v_a_swap, v_a, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	std_ptr_copy_nonoverlapping_Checked(&v_b_swap_tmp, v_b, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
}

/* sort9_optimal sorts nine elements by their optimal network. */
static void
STD_SORT_NAME(sort9_optimal) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	static const uint8 pairs[][2] = {
		{0, 3}, {1, 7}, {2, 5}, {4, 8}, {0, 7}, {2, 4}, {3, 8}, {5, 6}, {0, 2}, {1, 3}, {4, 5}, {7, 8},
		{1, 4}, {3, 6}, {5, 7}, {0, 1}, {2, 4}, {3, 5}, {6, 8}, {2, 3}, {4, 5}, {6, 7}, {1, 2}, {3, 4},
		{5, 6},
	};
	STD_SORT_T *v_base;

	if (len < 9)
		std_intrinsics_abort();
	v_base = v;
	for (size_t i = 0; i < lengthof(pairs); i++)
		STD_SORT_NAME(swap_if_less) (v_base, pairs[i][0], pairs[i][1], is_less);
}

/* sort13_optimal sorts thirteen elements by their optimal network. */
static void
STD_SORT_NAME(sort13_optimal) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	static const uint8 pairs[][2] = {
		{0, 12}, {1, 10}, {2, 9}, {3, 7}, {5, 11}, {6, 8}, {1, 6}, {2, 3}, {4, 11}, {7, 9}, {8, 10},
		{0, 4}, {1, 2}, {3, 6}, {7, 8}, {9, 10}, {11, 12}, {4, 6}, {5, 9}, {8, 11}, {10, 12}, {0, 5},
		{3, 8}, {4, 7}, {6, 11}, {9, 10}, {0, 1}, {2, 5}, {6, 9}, {7, 8}, {10, 11}, {1, 3}, {2, 4},
		{5, 6}, {9, 10}, {1, 2}, {3, 4}, {5, 7}, {6, 8}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {3, 4},
		{5, 6},
	};
	STD_SORT_T *v_base;

	if (len < 13)
		std_intrinsics_abort();
	v_base = v;
	for (size_t i = 0; i < lengthof(pairs); i++)
		STD_SORT_NAME(swap_if_less) (v_base, pairs[i][0], pairs[i][1], is_less);
}

#endif
#endif							/* STD_SORT_SHARED_ONLY */

/* STD_SORT_INSERT_TAIL_FRAME_0_T is insert_tail's state across its pad: the sift, the moved element, and its gap guard. */
#define STD_SORT_INSERT_TAIL_FRAME_0_T STD_SORT_NAME(insert_tail_frame_0_t)
typedef struct STD_SORT_INSERT_TAIL_FRAME_0_T
{
	STD_SORT_T *begin;
	STD_SORT_T *sift;
	StdSortLess *is_less;
	STD_SORT_T	tmp;
	STD_SORT_T *gap_guard_dst;
}			STD_SORT_INSERT_TAIL_FRAME_0_T;

/* insert_tail_body_0 is insert_tail's loop: each greater element shifted right into the gap, then the moved element written into it. */
static void
STD_SORT_NAME(insert_tail_body_0) (void *frame_)
{
	STD_SORT_INSERT_TAIL_FRAME_0_T *frame = frame_;

	for (;;)
	{
		std_ptr_copy_nonoverlapping_Checked(frame->sift, frame->gap_guard_dst, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
		frame->gap_guard_dst = frame->sift;
		if (frame->sift == frame->begin)
			break;
		frame->sift = frame->sift - 1;
		if (!STD_SORT_LESS(frame->is_less, &frame->tmp, frame->sift))
			break;
	}
	STD_SORT_NAME(CopyOnDrop_drop) (&frame->tmp, frame->gap_guard_dst, 1);
}

/* insert_tail_pad_0 is insert_tail's pad: the gap guard's drop, the moved element written into the gap. */
static void
STD_SORT_NAME(insert_tail_pad_0) (void *frame_)
{
	STD_SORT_INSERT_TAIL_FRAME_0_T *frame = frame_;

	STD_SORT_NAME(CopyOnDrop_drop) (&frame->tmp, frame->gap_guard_dst, 1);
}

/*
 * insert_tail moves the element at tail left until the elements from begin
 * to it are sorted, given they were sorted before it; a panic of is_less
 * writes the moved element, held in a temporary of T, into the gap.
 */
static void
STD_SORT_NAME(insert_tail) (STD_SORT_T * begin, STD_SORT_T * tail, StdSortLess *is_less)
{
	STD_SORT_INSERT_TAIL_FRAME_0_T frame = {.begin = begin, .sift = tail - 1, .is_less = is_less};

	if (!STD_SORT_LESS(is_less, tail, frame.sift))
		return;
	frame.tmp = *tail;
	frame.gap_guard_dst = tail;
	std_landing(&frame, STD_SORT_NAME(insert_tail_body_0), STD_SORT_NAME(insert_tail_pad_0));
}

/* insertion_sort_shift_left sorts a slice whose first offset elements are sorted, inserting the rest. */
static void
STD_SORT_NAME(insertion_sort_shift_left) (STD_SORT_T * v, size_t len, size_t offset, StdSortLess *is_less)
{
	STD_SORT_T *v_base;
	STD_SORT_T *v_end;
	STD_SORT_T *tail;

	if (offset == 0 || offset > len)
		std_intrinsics_abort();
	v_base = v;
	v_end = v_base + len;
	tail = v_base + offset;
	while (tail != v_end)
	{
		STD_SORT_NAME(insert_tail) (v_base, tail, is_less);
		tail = tail + 1;
	}
}

#if STD_SORT_FREEZE

/* sort4_stable sorts four elements stably into dst. */
static void
STD_SORT_NAME(sort4_stable) (const STD_SORT_T * v_base, STD_SORT_T * dst, StdSortLess *is_less)
{
	bool		c1 = STD_SORT_LESS(is_less, v_base + 1, v_base);
	bool		c2 = STD_SORT_LESS(is_less, v_base + 3, v_base + 2);
	const		STD_SORT_T *a = v_base + (c1 ? 1 : 0);
	const		STD_SORT_T *b = v_base + (c1 ? 0 : 1);
	const		STD_SORT_T *c = v_base + (2 + (c2 ? 1 : 0));
	const		STD_SORT_T *d = v_base + (2 + (c2 ? 0 : 1));
	bool		c3 = STD_SORT_LESS(is_less, c, a);
	bool		c4 = STD_SORT_LESS(is_less, d, b);
	const		STD_SORT_T *min = c3 ? c : a;
	const		STD_SORT_T *max = c4 ? b : d;
	const		STD_SORT_T *unknown_left = c3 ? a : (c4 ? c : b);
	const		STD_SORT_T *unknown_right = c4 ? d : (c3 ? b : c);
	bool		c5 = STD_SORT_LESS(is_less, unknown_right, unknown_left);
	const		STD_SORT_T *lo = c5 ? unknown_right : unknown_left;
	const		STD_SORT_T *hi = c5 ? unknown_left : unknown_right;

	std_ptr_copy_nonoverlapping_Checked(min, dst, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	std_ptr_copy_nonoverlapping_Checked(lo, dst + 1, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	std_ptr_copy_nonoverlapping_Checked(hi, dst + 2, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	std_ptr_copy_nonoverlapping_Checked(max, dst + 3, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
}

/* sort8_stable sorts eight elements stably into dst, through scratch. */
static void
STD_SORT_NAME(sort8_stable) (STD_SORT_T * v_base, STD_SORT_T * dst, STD_SORT_T * scratch_base, StdSortLess *is_less)
{
	STD_SORT_NAME(sort4_stable) (v_base, scratch_base, is_less);
	STD_SORT_NAME(sort4_stable) (v_base + 4, scratch_base + 4, is_less);
	std_slice_from_raw_parts_check(scratch_base, 8, STD_SORT_SIZE, STD_SORT_ALIGN, STD_SORT_SMALLSORT_FILE, 672, 29);
	STD_SORT_NAME(bidirectional_merge) (scratch_base, 8, dst, is_less);
}

/*
 * merge_up merges the next element of two sorted runs into dst from the
 * front, the left one on a tie, returning the three positions moved on.
 */
static void
STD_SORT_NAME(merge_up) (const STD_SORT_T * left_src, const STD_SORT_T * right_src, STD_SORT_T * dst,
						 StdSortLess *is_less, const STD_SORT_T * *result_0, const STD_SORT_T * *result_1,
						 STD_SORT_T * *result_2)
{
	bool		is_l = !STD_SORT_LESS(is_less, right_src, left_src);
	const		STD_SORT_T *src = is_l ? left_src : right_src;

	std_ptr_copy_nonoverlapping_Checked(src, dst, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	right_src = right_src + (size_t) !is_l;
	left_src = left_src + (size_t) is_l;
	dst = dst + 1;
	*result_0 = left_src;
	*result_1 = right_src;
	*result_2 = dst;
}

/*
 * merge_down merges the last element of two sorted runs into dst from the
 * back, the right one on a tie, returning the three positions moved on.
 */
static void
STD_SORT_NAME(merge_down) (const STD_SORT_T * left_src, const STD_SORT_T * right_src, STD_SORT_T * dst,
						   StdSortLess *is_less, const STD_SORT_T * *result_0, const STD_SORT_T * *result_1,
						   STD_SORT_T * *result_2)
{
	bool		is_l = !STD_SORT_LESS(is_less, right_src, left_src);
	const		STD_SORT_T *src = is_l ? right_src : left_src;

	std_ptr_copy_nonoverlapping_Checked(src, dst, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	right_src = (const STD_SORT_T *) ((uintptr_t) right_src - (size_t) is_l * STD_SORT_SIZE);
	left_src = (const STD_SORT_T *) ((uintptr_t) left_src - (size_t) !is_l * STD_SORT_SIZE);
	dst = dst - 1;
	*result_0 = left_src;
	*result_1 = right_src;
	*result_2 = dst;
}

/*
 * bidirectional_merge merges a slice's two sorted halves into dst from both
 * ends at once, panicking for a comparator that isn't a total order.
 */
static void
STD_SORT_NAME(bidirectional_merge) (const STD_SORT_T * v, size_t len, STD_SORT_T * dst, StdSortLess *is_less)
{
	const		STD_SORT_T *src = v;
	size_t		len_div_2 = len / 2;
	const		STD_SORT_T *left = src;
	const		STD_SORT_T *right = src + len_div_2;
	const		STD_SORT_T *left_rev = src + (len_div_2 - 1);
	const		STD_SORT_T *right_rev = src + (len - 1);
	STD_SORT_T *dst_rev = dst + (len - 1);
	const		STD_SORT_T *left_end;
	const		STD_SORT_T *right_end;

	for (size_t i = 0; i < len_div_2; i++)
	{
		STD_SORT_NAME(merge_up) (left, right, dst, is_less, &left, &right, &dst);
		STD_SORT_NAME(merge_down) (left_rev, right_rev, dst_rev, is_less, &left_rev, &right_rev, &dst_rev);
	}
	left_end = (const STD_SORT_T *) ((uintptr_t) left_rev + STD_SORT_SIZE);
	right_end = (const STD_SORT_T *) ((uintptr_t) right_rev + STD_SORT_SIZE);
	if (!std_is_multiple_of_usize(len, 2))
	{
		bool		left_nonempty = left < left_end;
		const		STD_SORT_T *last_src = left_nonempty ? left : right;

		std_ptr_copy_nonoverlapping_Checked(last_src, dst, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
		left = left + (size_t) left_nonempty;
		right = right + (size_t) !left_nonempty;
	}
	if (left != left_end || right != right_end)
		std_sort_panic_on_ord_violation();
}

#endif

#ifndef STD_SORT_SHARED_ONLY
/*
 * sort is core's unstable sort: nothing for a zero-sized T or fewer than
 * two elements, insertion sort up to twenty, and ipnsort above.
 */
STD_SORT_DEF void
STD_SORT_NAME(sort) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	if (STD_SORT_SIZE == 0)
		return;
	if (len < 2)
		return;
	if (len <= STD_SORT_MAX_LEN_ALWAYS_INSERTION_SORT)
	{
		STD_SORT_NAME(insertion_sort_shift_left) (v, len, 1, is_less);
		return;
	}
	STD_SORT_NAME(ipnsort) (v, len, is_less);
}

/*
 * ipnsort sorts a slice already sorted or reversed at once, and otherwise
 * by quicksort with a limit of 2 * floor(log2(len)) imbalanced partitions.
 */
static void
STD_SORT_NAME(ipnsort) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	bool		was_reversed;
	size_t		run_len = STD_SORT_NAME(find_existing_run) (v, len, is_less, &was_reversed);
	uint32		limit;

	if (run_len == len)
	{
		if (was_reversed)
			std_slice_reverse(v, len, STD_SORT_SIZE, STD_SORT_ALIGN);
		return;
	}
	limit = 2 * std_ilog2_usize(len | 1, STD_SORT_UNSTABLE_FILE, 136, 31);
	STD_SORT_NAME(quicksort) (v, len, NULL, limit, is_less);
}

/* heapsort sorts a slice by a heap, quicksort's fallback past its limit of imbalanced partitions. */
static void
STD_SORT_NAME(heapsort) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	for (size_t i_end = 0, i = len + len / 2; i > i_end;)
	{
		size_t		sift_idx;
		size_t		sub_len;

		i--;
		if (i >= len)
			sift_idx = i - len;
		else
		{
			STD_SORT_SWAP(v, len, 0, i, STD_SORT_HEAPSORT_FILE, 20, 15);
			sift_idx = 0;
		}
		sub_len = std_min_usize(i, len);
		std_range_to_check(sub_len, len, STD_SORT_HEAPSORT_FILE, 28, 29);
		STD_SORT_NAME(sift_down) (v, sub_len, sift_idx, is_less);
	}
}

/* sift_down moves a node of a heap down until it is no less than its children. */
static void
STD_SORT_NAME(sift_down) (STD_SORT_T * v, size_t len, size_t node, StdSortLess *is_less)
{
	STD_SORT_T *v_base = v;

	for (;;)
	{
		size_t		child = 2 * node + 1;

		if (child >= len)
			break;
		if (child + 1 < len)
			child += (size_t) STD_SORT_LESS(is_less, v_base + child, v_base + (child + 1));
		if (!STD_SORT_LESS(is_less, v_base + node, v_base + child))
			break;
		std_ptr_swap_nonoverlapping(v_base + node, v_base + child, 1, STD_SORT_SIZE, STD_SORT_ALIGN,
									STD_SORT_HEAPSORT_FILE, 72, 13);
		node = child;
	}
}

/* quicksort_closure_0 is quicksort's |a, b| !is_less(b, a), which partitions the elements equal to the ancestor pivot. */
static bool
STD_SORT_NAME(quicksort_closure_0) (void *captures, const void *a, const void *b)
{
	StdSortLess *is_less = (StdSortLess *) captures;

	return !STD_SORT_LESS(is_less, (const STD_SORT_T *) b, (const STD_SORT_T *) a);
}

/*
 * quicksort sorts a slice by partitions around chosen pivots, the elements
 * equal to an ancestor pivot partitioned off at once, a small slice by a
 * small sort, and a slice past the limit of imbalanced partitions by
 * heapsort.
 */
static void
STD_SORT_NAME(quicksort) (STD_SORT_T * v, size_t len, const STD_SORT_T * ancestor_pivot, uint32 limit,
						  StdSortLess *is_less)
{
	for (;;)
	{
		size_t		pivot_pos;
		size_t		num_lt;

		if (len <= STD_SORT_NAME(small_sort_threshold) ())
		{
			STD_SORT_NAME(small_sort) (v, len, is_less);
			return;
		}
		if (limit == 0)
		{
			STD_SORT_NAME(heapsort) (v, len, is_less);
			return;
		}
		limit -= 1;
		pivot_pos = STD_SORT_NAME(choose_pivot) (v, len, is_less);
		if (ancestor_pivot != NULL)
		{
			std_index_check(pivot_pos, len, STD_SORT_QUICKSORT_FILE, 51, 29);
			if (!STD_SORT_LESS(is_less, ancestor_pivot, v + pivot_pos))
			{
				StdSortLess closure = {STD_SORT_NAME(quicksort_closure_0), is_less};

				num_lt = STD_SORT_NAME(partition) (v, len, pivot_pos, &closure);
				std_range_from_check(num_lt + 1, len, STD_SORT_QUICKSORT_FILE, 56, 27);
				v = v + (num_lt + 1);
				len = len - (num_lt + 1);
				ancestor_pivot = NULL;
				continue;
			}
		}
		num_lt = STD_SORT_NAME(partition) (v, len, pivot_pos, is_less);
		{
			void	   *left;
			size_t		left_len;
			void	   *right;
			size_t		right_len;
			void	   *pivot;
			size_t		pivot_len;

			std_slice_split_at_mut(v, len, num_lt, STD_SORT_SIZE, STD_SORT_ALIGN, &left, &left_len, &right, &right_len,
								   STD_SORT_QUICKSORT_FILE, 68, 31);
			std_slice_split_at_mut(right, right_len, 1, STD_SORT_SIZE, STD_SORT_ALIGN, &pivot, &pivot_len, &right, &right_len,
								   STD_SORT_QUICKSORT_FILE, 69, 36);
			std_index_check(0, pivot_len, STD_SORT_QUICKSORT_FILE, 70, 22);
			STD_SORT_NAME(quicksort) ((STD_SORT_T *) left, left_len, ancestor_pivot, limit, is_less);
			v = (STD_SORT_T *) right;
			len = right_len;
			ancestor_pivot = (const STD_SORT_T *) pivot;
		}
	}
}

/*
 * partition moves the pivot to the front, partitions the rest around it,
 * and puts it between, returning its index.
 */
static size_t
STD_SORT_NAME(partition) (STD_SORT_T * v, size_t len, size_t pivot, StdSortLess *is_less)
{
	size_t		num_lt;

	if (len == 0)
		return 0;
	if (pivot >= len)
		std_intrinsics_abort();
	STD_SORT_SWAP_UNCHECKED(v, len, 0, pivot, STD_SORT_QUICKSORT_FILE, 111, 11);
	{
		void	   *pivot_slice;
		size_t		pivot_len;
		void	   *v_without_pivot;
		size_t		v_without_pivot_len;

		std_slice_split_at_mut(v, len, 1, STD_SORT_SIZE, STD_SORT_ALIGN, &pivot_slice, &pivot_len, &v_without_pivot,
							   &v_without_pivot_len, STD_SORT_QUICKSORT_FILE, 113, 38);
		std_index_check(0, pivot_len, STD_SORT_QUICKSORT_FILE, 120, 22);
#if STD_SORT_BRANCHLESS
		num_lt = STD_SORT_NAME(partition_lomuto_branchless_cyclic) ((STD_SORT_T *) v_without_pivot,
																	v_without_pivot_len,
																	(STD_SORT_T *) pivot_slice, is_less);
#else
		num_lt = STD_SORT_NAME(partition_hoare_branchy_cyclic) ((STD_SORT_T *) v_without_pivot, v_without_pivot_len,
																(STD_SORT_T *) pivot_slice, is_less);
#endif
	}
	if (num_lt >= len)
		std_intrinsics_abort();
	STD_SORT_SWAP_UNCHECKED(v, len, 0, num_lt, STD_SORT_QUICKSORT_FILE, 133, 11);
	return num_lt;
}

#if !STD_SORT_BRANCHLESS

/* STD_SORT_PARTITION_HOARE_BRANCHY_CYCLIC_FRAME_0_T is the partition's state across its pad: its arguments, its result, and its optional gap guard. */
#define STD_SORT_PARTITION_HOARE_BRANCHY_CYCLIC_FRAME_0_T STD_SORT_NAME(partition_hoare_branchy_cyclic_frame_0_t)
typedef struct STD_SORT_PARTITION_HOARE_BRANCHY_CYCLIC_FRAME_0_T
{
	STD_SORT_T *v;
	size_t		len;
	const		STD_SORT_T *pivot;
	StdSortLess *is_less;
	size_t		result;
	bool		gap_opt_has;
	STD_SORT_T *gap_pos;
	STD_SORT_T	gap_value;
}			STD_SORT_PARTITION_HOARE_BRANCHY_CYCLIC_FRAME_0_T;

/* partition_hoare_branchy_cyclic_body_0 is the partition's scans and cyclic swaps, then the gap guard's drop. */
static void
STD_SORT_NAME(partition_hoare_branchy_cyclic_body_0) (void *frame_)
{
	STD_SORT_PARTITION_HOARE_BRANCHY_CYCLIC_FRAME_0_T *frame = frame_;
	STD_SORT_T *v_base = frame->v;
	STD_SORT_T *left = v_base;
	STD_SORT_T *right = v_base + frame->len;

	for (;;)
	{
		bool		is_first_swap_pair;

		while (left < right && STD_SORT_LESS(frame->is_less, left, frame->pivot))
			left = left + 1;
		for (;;)
		{
			right = right - 1;
			if (left >= right || STD_SORT_LESS(frame->is_less, right, frame->pivot))
				break;
		}
		if (left >= right)
			break;
		is_first_swap_pair = !frame->gap_opt_has;
		if (is_first_swap_pair)
		{
			frame->gap_pos = right;
			frame->gap_value = *left;
			frame->gap_opt_has = true;
		}
		if (!frame->gap_opt_has)
			std_hint_unreachable_unchecked(STD_SORT_QUICKSORT_FILE, 217, 40);
		if (!is_first_swap_pair)
			std_ptr_copy_nonoverlapping_Checked(left, frame->gap_pos, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
		frame->gap_pos = right;
		std_ptr_copy_nonoverlapping_Checked(right, left, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
		left = left + 1;
	}
	frame->result = std_ptr_offset_from_unsigned_Checked(left, v_base, STD_SORT_SIZE, STD_SORT_QUICKSORT_FILE, 229, 14);
	if (frame->gap_opt_has)
	{
		frame->gap_opt_has = false;
		std_ptr_copy_nonoverlapping_Checked(&frame->gap_value, frame->gap_pos, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	}
}

/* partition_hoare_branchy_cyclic_pad_0 is the partition's pad: the gap guard's drop while it is live. */
static void
STD_SORT_NAME(partition_hoare_branchy_cyclic_pad_0) (void *frame_)
{
	STD_SORT_PARTITION_HOARE_BRANCHY_CYCLIC_FRAME_0_T *frame = frame_;

	if (frame->gap_opt_has)
		std_ptr_copy_nonoverlapping_Checked(&frame->gap_value, frame->gap_pos, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
}

/*
 * partition_hoare_branchy_cyclic partitions a slice of large elements
 * around a pivot by Hoare's scheme with a cyclic permutation, the gap guard
 * writing the element it holds in a temporary of T back should is_less
 * panic.
 */
static size_t
STD_SORT_NAME(partition_hoare_branchy_cyclic) (STD_SORT_T * v, size_t len, const STD_SORT_T * pivot,
											   StdSortLess *is_less)
{
	STD_SORT_PARTITION_HOARE_BRANCHY_CYCLIC_FRAME_0_T frame = {.v = v, .len = len, .pivot = pivot,
	.is_less = is_less};

	if (len == 0)
		return 0;
	std_landing(&frame, STD_SORT_NAME(partition_hoare_branchy_cyclic_body_0),
				STD_SORT_NAME(partition_hoare_branchy_cyclic_pad_0));
	return frame.result;
}

#else

/*
 * STD_SORT_PARTITION_STATE is the branchless cyclic partition's state: its
 * next element, its count below the pivot, and its gap.
 */
#define STD_SORT_PARTITION_STATE STD_SORT_CAT(StdSortPartitionState, STD_SORT_TS)
typedef struct STD_SORT_PARTITION_STATE
{
	STD_SORT_T *right;
	size_t		num_lt;
	STD_SORT_T *gap_pos;
	STD_SORT_T *gap_value;
}			STD_SORT_PARTITION_STATE;

/*
 * STD_SORT_PARTITION_CLOSURE_0 is the captures of the partition's loop_body
 * closure: v_base, pivot, and is_less, each by reference.
 */
#define STD_SORT_PARTITION_CLOSURE_0 STD_SORT_CAT(StdSortPartitionLomutoBranchlessCyclicClosure0, STD_SORT_TS)
typedef struct STD_SORT_PARTITION_CLOSURE_0
{
	STD_SORT_T **v_base;
	const		STD_SORT_T **pivot;
	StdSortLess **is_less;
}			STD_SORT_PARTITION_CLOSURE_0;

/*
 * partition_lomuto_branchless_cyclic_closure_0 is the partition's loop
 * body: one element moved through the gap.
 */
static void
STD_SORT_NAME(partition_lomuto_branchless_cyclic_closure_0) (void *captures, STD_SORT_PARTITION_STATE * state)
{
	STD_SORT_PARTITION_CLOSURE_0 *c = captures;
	STD_SORT_T *v_base = *c->v_base;
	const		STD_SORT_T *pivot = *c->pivot;
	StdSortLess *is_less = *c->is_less;
	bool		right_is_lt = STD_SORT_LESS(is_less, state->right, pivot);
	STD_SORT_T *left = v_base + state->num_lt;

	std_ptr_copy(left, state->gap_pos, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	std_ptr_copy_nonoverlapping_Checked(state->right, left, 1, STD_SORT_SIZE, STD_SORT_ALIGN);
	state->gap_pos = state->right;
	state->num_lt += (size_t) right_is_lt;
	state->right = state->right + 1;
}

/* STD_SORT_PARTITION_LOMUTO_BRANCHLESS_CYCLIC_FRAME_0_T is the partition's state across its pad: its loop's bounds and closure, and its gap state. */
#define STD_SORT_PARTITION_LOMUTO_BRANCHLESS_CYCLIC_FRAME_0_T STD_SORT_NAME(partition_lomuto_branchless_cyclic_frame_0_t)
typedef struct STD_SORT_PARTITION_LOMUTO_BRANCHLESS_CYCLIC_FRAME_0_T
{
	STD_SORT_T *v_base;
	size_t		len;
	size_t		unroll_len;
	STD_SORT_T *unroll_end;
	STD_SORT_PARTITION_CLOSURE_0 *loop_body;
	STD_SORT_PARTITION_STATE state;
}			STD_SORT_PARTITION_LOMUTO_BRANCHLESS_CYCLIC_FRAME_0_T;

/* partition_lomuto_branchless_cyclic_body_0 is the partition's unrolled loop, then its tail through the gap. */
static void
STD_SORT_NAME(partition_lomuto_branchless_cyclic_body_0) (void *frame_)
{
	STD_SORT_PARTITION_LOMUTO_BRANCHLESS_CYCLIC_FRAME_0_T *frame = frame_;
	STD_SORT_T *end;

	while (frame->state.right < frame->unroll_end)
	{
		if (frame->unroll_len == 2)
		{
			STD_SORT_NAME(partition_lomuto_branchless_cyclic_closure_0) (frame->loop_body, &frame->state);
			STD_SORT_NAME(partition_lomuto_branchless_cyclic_closure_0) (frame->loop_body, &frame->state);
		}
		else
			STD_SORT_NAME(partition_lomuto_branchless_cyclic_closure_0) (frame->loop_body, &frame->state);
	}
	end = frame->v_base + frame->len;
	for (;;)
	{
		bool		is_done = frame->state.right == end;

		frame->state.right = is_done ? frame->state.gap_value : frame->state.right;
		STD_SORT_NAME(partition_lomuto_branchless_cyclic_closure_0) (frame->loop_body, &frame->state);
		if (is_done)
			break;
	}
}

/* partition_lomuto_branchless_cyclic_pad_0 is the partition's pad: the gap guard's drop, the held element written into the gap. */
static void
STD_SORT_NAME(partition_lomuto_branchless_cyclic_pad_0) (void *frame_)
{
	STD_SORT_PARTITION_LOMUTO_BRANCHLESS_CYCLIC_FRAME_0_T *frame = frame_;

	std_ptr_copy_nonoverlapping_Checked(frame->state.gap_value, frame->state.gap_pos, 1, STD_SORT_SIZE,
										STD_SORT_ALIGN);
}

/*
 * partition_lomuto_branchless_cyclic partitions a slice of small elements
 * around a pivot by Lomuto's scheme without branches, through a cyclic gap
 * whose element a temporary of T holds, the gap guard writing it back
 * should is_less panic.
 */
static size_t
STD_SORT_NAME(partition_lomuto_branchless_cyclic) (STD_SORT_T * v, size_t len, const STD_SORT_T * pivot,
												   StdSortLess *is_less)
{
	STD_SORT_T *v_base = v;
	STD_SORT_T	gap_value;
	STD_SORT_PARTITION_CLOSURE_0 loop_body;
	STD_SORT_PARTITION_LOMUTO_BRANCHLESS_CYCLIC_FRAME_0_T frame;

	if (len == 0)
		return 0;
	loop_body.v_base = &v_base;
	loop_body.pivot = &pivot;
	loop_body.is_less = &is_less;
	gap_value = *v_base;
	frame.v_base = v_base;
	frame.len = len;
	frame.loop_body = &loop_body;
	frame.state.num_lt = 0;
	frame.state.right = v_base + 1;
	frame.state.gap_pos = v_base;
	frame.state.gap_value = &gap_value;
	frame.unroll_len = STD_SORT_SIZE <= 16 ? 2 : 1;
	frame.unroll_end = v_base + (len - (frame.unroll_len - 1));
	std_landing(&frame, STD_SORT_NAME(partition_lomuto_branchless_cyclic_body_0),
				STD_SORT_NAME(partition_lomuto_branchless_cyclic_pad_0));
	return frame.state.num_lt;
}

#endif							/* STD_SORT_BRANCHLESS */

#ifndef STD_SORT_WITHOUT_UNSTABLE

/* sort_unstable is <[T]>::sort_unstable: core's unstable sort with T's PartialOrd::lt. */
STD_SORT_DEF void
STD_SORT_UNSTABLE(STD_SORT_T * v, size_t len)
{
	StdSortLess lt = {STD_SORT_LT, NULL};

	STD_SORT_NAME(sort) (v, len, &lt);
}

#endif							/* STD_SORT_WITHOUT_UNSTABLE */

#ifdef STD_SORT_WITH_BY

/* STD_SORT_SORT_UNSTABLE_BY_FRAME_0_T is sort_unstable_by's state across its pad: the slice, compare's captures, drop, and liveness, and is_less. */
#define STD_SORT_SORT_UNSTABLE_BY_FRAME_0_T STD_SORT_NAME(sort_unstable_by_frame_0_t)
typedef struct STD_SORT_SORT_UNSTABLE_BY_FRAME_0_T
{
	STD_SORT_T *v;
	size_t		len;
	void	   *compare_captures;
	void		(*compare_drop) (void *);
	bool		compare_live;
	StdSortLess is_less;
}			STD_SORT_SORT_UNSTABLE_BY_FRAME_0_T;

/* sort_unstable_by_body_0 is sort_unstable_by's sort, then compare's drop. */
static void
STD_SORT_NAME(sort_unstable_by_body_0) (void *frame_)
{
	STD_SORT_SORT_UNSTABLE_BY_FRAME_0_T *frame = frame_;

	STD_SORT_NAME(sort) (frame->v, frame->len, &frame->is_less);
	frame->compare_live = false;
	if (frame->compare_drop != NULL)
		frame->compare_drop(frame->compare_captures);
}

/* sort_unstable_by_pad_0 is sort_unstable_by's pad: compare's drop while it is live. */
static void
STD_SORT_NAME(sort_unstable_by_pad_0) (void *frame_)
{
	STD_SORT_SORT_UNSTABLE_BY_FRAME_0_T *frame = frame_;

	if (frame->compare_live && frame->compare_drop != NULL)
		frame->compare_drop(frame->compare_captures);
}

/*
 * sort_unstable_by is <[T]>::sort_unstable_by: core's unstable sort, one
 * element before another where compare orders it Less. It takes compare and
 * drops it at the end, or should the sort panic.
 */
STD_SORT_DEF void
STD_SORT_UNSTABLE_BY(STD_SORT_T * v, size_t len, StdOrdering (*compare) (void *, const void *, const void *),
					 void *compare_captures, void (*compare_drop) (void *))
{
	StdSortCompare by = {compare, compare_captures};
	STD_SORT_SORT_UNSTABLE_BY_FRAME_0_T frame = {.v = v, .len = len, .compare_captures = compare_captures,
	.compare_drop = compare_drop, .compare_live = true, .is_less = {std_slice_sort_unstable_by_is_less, &by}};

	std_landing(&frame, STD_SORT_NAME(sort_unstable_by_body_0), STD_SORT_NAME(sort_unstable_by_pad_0));
}

#endif							/* STD_SORT_WITH_BY */
#endif							/* STD_SORT_SHARED_ONLY */

#if defined(STD_SORT_WITH_PARTIAL) && !defined(STD_SORT_SHARED_ONLY)

/* STD_SORT_PARTIAL is <[T]>::partial_sort_unstable's instance. */
#define STD_SORT_PARTIAL STD_SORT_CAT(std_slice_partial_sort_unstable_, STD_SORT_TS)

/* STD_SORT_INSERTION_SORT_THRESHOLD is select.rs' INSERTION_SORT_THRESHOLD, below which a sub-slice is insertion sorted. */
#define STD_SORT_INSERTION_SORT_THRESHOLD 16

static void STD_SORT_NAME(median_of_medians) (STD_SORT_T * v, size_t len, StdSortLess *is_less, size_t k);

/*
 * min_index is select.rs' min_index: the index of the first minimum,
 * enumerate's reduce keeping the accumulator unless the next is less, or
 * false for an empty slice.
 */
static bool
STD_SORT_NAME(min_index) (const STD_SORT_T * v, size_t len, StdSortLess *is_less, size_t *result)
{
	size_t		acc;

	if (len == 0)
		return false;
	acc = 0;
	for (size_t i = 1; i < len; i++)
		if (STD_SORT_LESS(is_less, v + i, v + acc))
			acc = i;
	*result = acc;
	return true;
}

/*
 * max_index is select.rs' max_index: the index of the last maximum,
 * enumerate's reduce taking the next whenever the accumulator is less, or
 * false for an empty slice.
 */
static bool
STD_SORT_NAME(max_index) (const STD_SORT_T * v, size_t len, StdSortLess *is_less, size_t *result)
{
	size_t		acc;

	if (len == 0)
		return false;
	acc = 0;
	for (size_t i = 1; i < len; i++)
		if (STD_SORT_LESS(is_less, v + acc, v + i))
			acc = i;
	*result = acc;
	return true;
}

/* median_idx is select.rs' median_idx: the index of the median of v[a], v[b], and v[c]. */
static size_t
STD_SORT_NAME(median_idx) (const STD_SORT_T * v, size_t len, StdSortLess *is_less, size_t a, size_t b, size_t c)
{
	std_index_check(c, len, STD_SORT_SELECT_FILE, 300, 17);
	std_index_check(a, len, STD_SORT_SELECT_FILE, 300, 24);
	if (STD_SORT_LESS(is_less, v + c, v + a))
	{
		size_t		tmp = a;

		a = c;
		c = tmp;
	}
	std_index_check(c, len, STD_SORT_SELECT_FILE, 303, 17);
	std_index_check(b, len, STD_SORT_SELECT_FILE, 303, 24);
	if (STD_SORT_LESS(is_less, v + c, v + b))
		return c;
	std_index_check(b, len, STD_SORT_SELECT_FILE, 306, 17);
	std_index_check(a, len, STD_SORT_SELECT_FILE, 306, 24);
	if (STD_SORT_LESS(is_less, v + b, v + a))
		return a;
	return b;
}

/*
 * ninther is select.rs' ninther: the nine elements at a through i moved so
 * that v[e] holds their median and the others are partitioned around it.
 */
static void
STD_SORT_NAME(ninther) (STD_SORT_T * v, size_t len, StdSortLess *is_less, size_t a, size_t b, size_t c, size_t d,
						size_t e, size_t f, size_t g, size_t h, size_t i)
{
	b = STD_SORT_NAME(median_idx) (v, len, is_less, a, b, c);
	h = STD_SORT_NAME(median_idx) (v, len, is_less, g, h, i);
	std_index_check(h, len, STD_SORT_SELECT_FILE, 264, 17);
	std_index_check(b, len, STD_SORT_SELECT_FILE, 264, 24);
	if (STD_SORT_LESS(is_less, v + h, v + b))
	{
		size_t		tmp = b;

		b = h;
		h = tmp;
	}
	std_index_check(f, len, STD_SORT_SELECT_FILE, 267, 17);
	std_index_check(d, len, STD_SORT_SELECT_FILE, 267, 24);
	if (STD_SORT_LESS(is_less, v + f, v + d))
	{
		size_t		tmp = d;

		d = f;
		f = tmp;
	}
	std_index_check(e, len, STD_SORT_SELECT_FILE, 270, 17);
	std_index_check(d, len, STD_SORT_SELECT_FILE, 270, 24);
	if (STD_SORT_LESS(is_less, v + e, v + d))
	{
	}
	else
	{
		std_index_check(f, len, STD_SORT_SELECT_FILE, 272, 24);
		std_index_check(e, len, STD_SORT_SELECT_FILE, 272, 31);
		if (STD_SORT_LESS(is_less, v + f, v + e))
			d = f;
		else
		{
			std_index_check(e, len, STD_SORT_SELECT_FILE, 275, 21);
			std_index_check(b, len, STD_SORT_SELECT_FILE, 275, 28);
			if (STD_SORT_LESS(is_less, v + e, v + b))
				STD_SORT_SWAP(v, len, e, b, STD_SORT_SELECT_FILE, 276, 15);
			else
			{
				std_index_check(h, len, STD_SORT_SELECT_FILE, 277, 28);
				std_index_check(e, len, STD_SORT_SELECT_FILE, 277, 35);
				if (STD_SORT_LESS(is_less, v + h, v + e))
					STD_SORT_SWAP(v, len, e, h, STD_SORT_SELECT_FILE, 278, 15);
			}
			return;
		}
	}
	std_index_check(d, len, STD_SORT_SELECT_FILE, 282, 17);
	std_index_check(b, len, STD_SORT_SELECT_FILE, 282, 24);
	if (STD_SORT_LESS(is_less, v + d, v + b))
		d = b;
	else
	{
		std_index_check(h, len, STD_SORT_SELECT_FILE, 284, 24);
		std_index_check(d, len, STD_SORT_SELECT_FILE, 284, 31);
		if (STD_SORT_LESS(is_less, v + h, v + d))
			d = h;
	}
	STD_SORT_SWAP(v, len, d, e, STD_SORT_SELECT_FILE, 288, 7);
}

/*
 * median_of_ninthers is select.rs' median_of_ninthers: a pivot near the
 * slice's median, by ninthers over a fraction of it, then the slice
 * partitioned around it, returning its index.
 */
static size_t
STD_SORT_NAME(median_of_ninthers) (STD_SORT_T * v, size_t len, StdSortLess *is_less)
{
	size_t		frac;
	size_t		pivot;
	size_t		lo;
	size_t		hi;
	size_t		gap;
	size_t		a;
	size_t		b;

	if (len <= 1024)
		frac = len / 12;
	else if (len <= std_saturating_mul_usize(128, 1024))
		frac = len / 64;
	else
		frac = len / 1024;
	pivot = frac / 2;
	lo = len / 2 - pivot;
	hi = frac + lo;
	gap = (len - 9 * frac) / 4;
	a = lo - 4 * frac - gap;
	b = hi + gap;
	for (size_t i = lo; i < hi; i++)
	{
		STD_SORT_NAME(ninther) (v, len, is_less, a, i - frac, b, a + 1, i, b + 1, a + 2, i + frac, b + 2);
		a += 3;
		b += 3;
	}
	std_range_check(lo, lo + frac, len, STD_SORT_SELECT_FILE, 241, 29);
	STD_SORT_NAME(median_of_medians) (v + lo, frac, is_less, pivot);
	return STD_SORT_NAME(partition) (v, len, lo + pivot, is_less);
}

/*
 * median_of_medians is select.rs' median_of_medians: the k-th element moved
 * to its sorted place in guaranteed linear time, a quickselect over
 * median_of_ninthers' pivots.
 */
static void
STD_SORT_NAME(median_of_medians) (STD_SORT_T * v, size_t len, StdSortLess *is_less, size_t k)
{
	for (;;)
	{
		size_t		p;

		if (len <= STD_SORT_INSERTION_SORT_THRESHOLD)
		{
			if (len >= 2)
				STD_SORT_NAME(insertion_sort_shift_left) (v, len, 1, is_less);
			return;
		}
		if (k == len - 1)
		{
			size_t		max_idx;

			if (!STD_SORT_NAME(max_index) (v, len, is_less, &max_idx))
				std_option_unwrap_failed(STD_SORT_SELECT_FILE, 190, 49);
			STD_SORT_SWAP(v, len, max_idx, k, STD_SORT_SELECT_FILE, 191, 15);
			return;
		}
		else if (k == 0)
		{
			size_t		min_idx;

			if (!STD_SORT_NAME(min_index) (v, len, is_less, &min_idx))
				std_option_unwrap_failed(STD_SORT_SELECT_FILE, 196, 49);
			STD_SORT_SWAP(v, len, min_idx, k, STD_SORT_SELECT_FILE, 197, 15);
			return;
		}
		p = STD_SORT_NAME(median_of_ninthers) (v, len, is_less);
		if (p == k)
			return;
		else if (p > k)
		{
			std_range_to_check(p, len, STD_SORT_SELECT_FILE, 206, 23);
			len = p;
		}
		else
		{
			std_range_from_check(p + 1, len, STD_SORT_SELECT_FILE, 210, 23);
			v = v + (p + 1);
			len = len - (p + 1);
			k -= p + 1;
		}
	}
}

/* select_closure_0 is partition_at_index_loop's closure |a, b| !is_less(b, a), over the loop's is_less. */
static bool
STD_SORT_NAME(select_closure_0) (void *captures, const void *a, const void *b)
{
	StdSortLess *is_less = captures;

	return !STD_SORT_LESS(is_less, b, a);
}

/*
 * partition_at_index_loop is select.rs' introselect: quickselect with
 * choose_pivot's pivots and the ancestor pivot's equal partition, falling
 * back to median_of_medians after sixteen rounds.
 */
static void
STD_SORT_NAME(partition_at_index_loop) (STD_SORT_T * v, size_t len, size_t index, const STD_SORT_T * ancestor_pivot,
										StdSortLess *is_less)
{
	uint32		limit = 16;

	for (;;)
	{
		size_t		pivot_pos;
		size_t		mid;

		if (len <= STD_SORT_INSERTION_SORT_THRESHOLD)
		{
			if (len >= 2)
				STD_SORT_NAME(insertion_sort_shift_left) (v, len, 1, is_less);
			return;
		}
		if (limit == 0)
		{
			STD_SORT_NAME(median_of_medians) (v, len, is_less, index);
			return;
		}
		limit -= 1;
		pivot_pos = STD_SORT_NAME(choose_pivot) (v, len, is_less);
		if (ancestor_pivot != NULL)
		{
			std_index_check(pivot_pos, len, STD_SORT_SELECT_FILE, 104, 26);
			if (!STD_SORT_LESS(is_less, ancestor_pivot, v + pivot_pos))
			{
				StdSortLess closure = {STD_SORT_NAME(select_closure_0), is_less};
				size_t		num_lt = STD_SORT_NAME(partition) (v, len, pivot_pos, &closure);

				mid = num_lt + 1;
				if (mid > index)
					return;
				std_range_from_check(mid, len, STD_SORT_SELECT_FILE, 118, 27);
				v = v + mid;
				len = len - mid;
				index = index - mid;
				ancestor_pivot = NULL;
				continue;
			}
		}
		mid = STD_SORT_NAME(partition) (v, len, pivot_pos, is_less);
		{
			void	   *left;
			size_t		left_len;
			void	   *right;
			size_t		right_len;
			void	   *pivot;
			size_t		pivot_len;

			std_slice_split_at_mut(v, len, mid, STD_SORT_SIZE, STD_SORT_ALIGN, &left, &left_len, &right, &right_len,
								   STD_SORT_SELECT_FILE, 128, 31);
			std_slice_split_at_mut(right, right_len, 1, STD_SORT_SIZE, STD_SORT_ALIGN, &pivot, &pivot_len, &right,
								   &right_len, STD_SORT_SELECT_FILE, 129, 36);
			std_index_check(0, pivot_len, STD_SORT_SELECT_FILE, 130, 22);
			if (mid < index)
			{
				v = (STD_SORT_T *) right;
				len = right_len;
				index = index - mid - 1;
				ancestor_pivot = (const STD_SORT_T *) pivot;
			}
			else if (mid > index)
			{
				v = (STD_SORT_T *) left;
				len = left_len;
			}
			else
				return;
		}
	}
}

/*
 * partition_at_index is select.rs' partition_at_index: the element at index
 * moved to its sorted place, those before it no greater and those after no
 * less, returning the slices before and after it.
 */
static void
STD_SORT_NAME(partition_at_index) (STD_SORT_T * v, size_t len, size_t index, StdSortLess *is_less,
								   STD_SORT_T * *left, size_t *left_len, STD_SORT_T * *right, size_t *right_len)
{
	void	   *left_v;
	void	   *right_v;
	void	   *pivot;
	size_t		pivot_len;

	if (index >= len)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("partition_at_index index "), STD_FMT_ARG(0),
		STD_FMT_LIT(" greater than length of slice "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&index, std_usize_Display_fmt}, {&len, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(STD_SORT_SELECT_FILE, 29, 9, &arguments);
	}
	if (index == len - 1)
	{
		size_t		max_idx;

		if (!STD_SORT_NAME(max_index) (v, len, is_less, &max_idx))
			std_option_unwrap_failed(STD_SORT_SELECT_FILE, 37, 50);
		STD_SORT_SWAP(v, len, max_idx, index, STD_SORT_SELECT_FILE, 38, 11);
	}
	else if (index == 0)
	{
		size_t		min_idx;

		if (!STD_SORT_NAME(min_index) (v, len, is_less, &min_idx))
			std_option_unwrap_failed(STD_SORT_SELECT_FILE, 42, 50);
		STD_SORT_SWAP(v, len, min_idx, index, STD_SORT_SELECT_FILE, 43, 11);
	}
	else
		STD_SORT_NAME(partition_at_index_loop) (v, len, index, NULL, is_less);
	std_slice_split_at_mut(v, len, index, STD_SORT_SIZE, STD_SORT_ALIGN, &left_v, left_len, &right_v, right_len,
						   STD_SORT_SELECT_FILE, 55, 27);
	std_slice_split_at_mut(right_v, *right_len, 1, STD_SORT_SIZE, STD_SORT_ALIGN, &pivot, &pivot_len, &right_v,
						   right_len, STD_SORT_SELECT_FILE, 56, 32);
	std_index_check(0, pivot_len, STD_SORT_SELECT_FILE, 57, 22);
	*left = left_v;
	*right = right_v;
}

/*
 * partial_sort is unstable/mod.rs' partial_sort: the range's elements
 * sorted in place, those before them no greater and those after no less,
 * by at most two partition_at_index calls around the range, then sort.
 */
static void
STD_SORT_NAME(partial_sort) (STD_SORT_T * v, size_t len, StdBound range_start, StdBound range_end,
							 StdSortLess *is_less)
{
	size_t		start;
	size_t		end;
	STD_SORT_T *left;
	size_t		left_len;
	STD_SORT_T *right;
	size_t		right_len;

	std_slice_range(range_start, range_end, len, &start, &end);
	if (end - start <= 1)
	{
		if (end == 0 || start == len)
			return;
		STD_SORT_NAME(partition_at_index) (v, len, start, is_less, &left, &left_len, &right, &right_len);
		return;
	}
	if (end + 8 <= len)
	{
		STD_SORT_NAME(partition_at_index) (v, len, end - 1, is_less, &left, &left_len, &right, &right_len);
		v = left;
		len = left_len;
	}
	if (start >= 8)
	{
		STD_SORT_NAME(partition_at_index) (v, len, start, is_less, &left, &left_len, &right, &right_len);
		v = right;
		len = right_len;
	}
	STD_SORT_NAME(sort) (v, len, is_less);
}

/* partial_sort_unstable is <[T]>::partial_sort_unstable: partial_sort with T's PartialOrd::lt. */
STD_SORT_DEF void
STD_SORT_PARTIAL(STD_SORT_T * v, size_t len, StdBound start, StdBound end)
{
	StdSortLess is_less = {STD_SORT_LT, NULL};

	STD_SORT_NAME(partial_sort) (v, len, start, end, &is_less);
}

#undef STD_SORT_INSERTION_SORT_THRESHOLD
#endif							/* STD_SORT_WITH_PARTIAL */

#undef STD_SORT_SIZE
#undef STD_SORT_ALIGN
#undef STD_SORT_SWAP
#undef STD_SORT_SWAP_UNCHECKED
#undef STD_SORT_PARTITION_STATE
#undef STD_SORT_PARTITION_CLOSURE_0
#undef STD_SORT_SMALL_SORT_GENERAL_WITH_SCRATCH_FRAME_0_T
#undef STD_SORT_INSERT_TAIL_FRAME_0_T
#undef STD_SORT_PARTITION_HOARE_BRANCHY_CYCLIC_FRAME_0_T
#undef STD_SORT_PARTITION_LOMUTO_BRANCHLESS_CYCLIC_FRAME_0_T
#undef STD_SORT_SORT_UNSTABLE_BY_FRAME_0_T

#endif							/* STD_SORT_DEFINE */
#endif							/* STD_SORT_UNIT */

#undef STD_SORT_CAT_
#undef STD_SORT_CAT
#undef STD_SORT_DEF_static
#undef STD_SORT_DEF_extern
#undef STD_SORT_DEF
#undef STD_SORT_NAME
#undef STD_SORT_UNSTABLE
#undef STD_SORT_UNSTABLE_BY
#undef STD_SORT_WITH_BY
#undef STD_SORT_WITHOUT_UNSTABLE
#undef STD_SORT_SCOPE
#undef STD_SORT_DECLARE
#undef STD_SORT_DEFINE
#undef STD_SORT_TS
#undef STD_SORT_T
#undef STD_SORT_FREEZE
#undef STD_SORT_COPY
#undef STD_SORT_LT
#undef STD_SORT_BRANCHLESS
#undef STD_SORT_SHARED_ONLY
#undef STD_SORT_WITH_PARTIAL
#undef STD_SORT_PARTIAL
#undef STD_SORT_UNIT
