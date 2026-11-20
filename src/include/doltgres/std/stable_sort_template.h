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
 * The template of slices' stable sort (the port text's sections 2 and 9): a
 * port of core/src/slice/sort/stable at Rust 1.99.1, driftsort, in its
 * modules' order, at an element type T: sort's insertion sort for short
 * slices and driftsort_main's scratch on the stack or the heap, drift's run
 * detection and its merge tree, the stable quicksort with its ancestor pivot
 * and its stable partition, and merge with its MergeState, so the
 * comparator runs on the same pairs in the same order as the Rust's. It
 * calls the parts core shares with the unstable sort (find_existing_run,
 * choose_pivot, the insertion sort, and the stable small sort) from
 * sort_template.h's instance at the same T, which the translation unit
 * includes first with STD_SORT_SHARED_ONLY. MergeState's drop and the heap
 * scratch's are landing pads. A crate's sort is codegened in the crate, so
 * its pointer copies run their precondition checks; Rust's std is built
 * without overflow checks, so its arithmetic here is plain C's, and its
 * intrinsics::abort is abort.
 *
 * STD_STABLE_SORT_SCOPE is static or extern, STD_STABLE_SORT_DECLARE and
 * STD_STABLE_SORT_DEFINE choose the declarations and definitions,
 * STD_STABLE_SORT_TS spells T as sort_template.h's STD_SORT_TS did,
 * STD_STABLE_SORT_T is T, STD_STABLE_SORT_FREEZE is 1 when T is Freeze,
 * StableSmallSortTypeImpl's choice, and STD_STABLE_SORT_LT is T's
 * PartialOrd::lt's FnMut shim, a StdIsLess, which sort passes;
 * STD_STABLE_SORT_WITH_BY, when defined, adds sort_by's instance, and
 * STD_STABLE_SORT_WITHOUT_SORT leaves sort's out for an instance that sorts
 * only by a comparison, which then needs no STD_STABLE_SORT_LT;
 * STD_STABLE_SORT_WITH_BY_KEY, when
 * defined, adds sort_by_key's instance at a key K whose drop does nothing,
 * which STD_STABLE_SORT_KEY_KS spells, STD_STABLE_SORT_KEY_K is, and
 * STD_STABLE_SORT_KEY_LT(a, b) compares by K's PartialOrd::lt on pointers to
 * keys; and STD_STABLE_SORT_UNIT, the unit switch,
 * makes T () with no storage, whose sort is a function of the length alone
 * that returns at once. Every parameter is undefined at the end.
 */

/* STD_STABLE_SORT_CAT pastes two tokens after expanding them. */
#define STD_STABLE_SORT_CAT_(a, b) a##b
#define STD_STABLE_SORT_CAT(a, b) STD_STABLE_SORT_CAT_(a, b)

/* STD_STABLE_SORT_DEF is STD_STABLE_SORT_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_STABLE_SORT_DEF_static static
#define STD_STABLE_SORT_DEF_extern
#define STD_STABLE_SORT_DEF STD_STABLE_SORT_CAT(STD_STABLE_SORT_DEF_, STD_STABLE_SORT_SCOPE)

/* STD_STABLE_SORT_NAME names the stable sort's functions at T. */
#define STD_STABLE_SORT_NAME(name) STD_STABLE_SORT_CAT(std_stable_sort_##name##_, STD_STABLE_SORT_TS)

/* STD_STABLE_SORT_SHARED names a function of sort_template.h's instance at T. */
#define STD_STABLE_SORT_SHARED(name) STD_STABLE_SORT_CAT(std_sort_##name##_, STD_STABLE_SORT_TS)

/* STD_STABLE_SORT_SLICE is <[T]>::sort's instance. */
#define STD_STABLE_SORT_SLICE STD_STABLE_SORT_CAT(std_slice_sort_, STD_STABLE_SORT_TS)

/* STD_STABLE_SORT_SLICE_BY is <[T]>::sort_by's instance. */
#define STD_STABLE_SORT_SLICE_BY STD_STABLE_SORT_CAT(std_slice_sort_by_, STD_STABLE_SORT_TS)

/* STD_STABLE_SORT_SLICE_BY_KEY is <[T]>::sort_by_key's instance at K. */
#define STD_STABLE_SORT_SLICE_BY_KEY \
	STD_STABLE_SORT_CAT(STD_STABLE_SORT_CAT(std_slice_sort_by_key_, STD_STABLE_SORT_TS), STD_STABLE_SORT_KEY_KS)

#ifdef STD_STABLE_SORT_UNIT

#ifdef STD_STABLE_SORT_DECLARE
STD_STABLE_SORT_SCOPE void STD_STABLE_SORT_SLICE(size_t len);
#endif

#ifdef STD_STABLE_SORT_DEFINE

/* <[()]>::sort is core's stable sort at the zero-sized (), which returns at once. */
STD_STABLE_SORT_DEF void
STD_STABLE_SORT_SLICE(size_t len)
{
}

#endif

#else							/* STD_STABLE_SORT_UNIT */

#ifdef STD_STABLE_SORT_DECLARE
#ifndef STD_STABLE_SORT_WITHOUT_SORT
STD_STABLE_SORT_SCOPE void STD_STABLE_SORT_SLICE(STD_STABLE_SORT_T * v, size_t len);
#endif
#ifdef STD_STABLE_SORT_WITH_BY
STD_STABLE_SORT_SCOPE void STD_STABLE_SORT_SLICE_BY(STD_STABLE_SORT_T * v, size_t len,
													StdOrdering (*compare) (void *, const void *, const void *),
													void *compare_captures, void (*compare_drop) (void *));
#endif
#ifdef STD_STABLE_SORT_WITH_BY_KEY
STD_STABLE_SORT_SCOPE void STD_STABLE_SORT_SLICE_BY_KEY(STD_STABLE_SORT_T * v, size_t len,
														STD_STABLE_SORT_KEY_K(*f) (void *, const void *),
														void *f_captures, void (*f_drop) (void *));
#endif
STD_STABLE_SORT_SCOPE void STD_STABLE_SORT_NAME(sort) (STD_STABLE_SORT_T * v, size_t len, StdSortLess *is_less);
#endif

#ifdef STD_STABLE_SORT_DEFINE

/* STD_STABLE_SORT_SIZE and STD_STABLE_SORT_ALIGN are T's size and alignment, which its pointer copies take. */
#define STD_STABLE_SORT_SIZE sizeof(STD_STABLE_SORT_T)
#define STD_STABLE_SORT_ALIGN _Alignof(STD_STABLE_SORT_T)

/* STD_STABLE_SORT_DIR is core's slice/sort/stable, where the stable sort's own panics report. */
#define STD_STABLE_SORT_DIR STD_SORT_DIR "stable" STD_SEP

/* STD_STABLE_SORT_DRIFT_FILE is core's slice/sort/stable/drift.rs. */
#define STD_STABLE_SORT_DRIFT_FILE STD_STABLE_SORT_DIR "drift.rs"

/* STD_STABLE_SORT_QUICKSORT_FILE is core's slice/sort/stable/quicksort.rs. */
#define STD_STABLE_SORT_QUICKSORT_FILE STD_STABLE_SORT_DIR "quicksort.rs"

/* STD_STABLE_SORT_MERGE_FILE is core's slice/sort/stable/merge.rs. */
#define STD_STABLE_SORT_MERGE_FILE STD_STABLE_SORT_DIR "merge.rs"

/* STD_STABLE_SORT_MOD_FILE is core's slice/sort/stable/mod.rs, where AlignedStorage's slice is made. */
#define STD_STABLE_SORT_MOD_FILE STD_STABLE_SORT_DIR "mod.rs"

/* STD_STABLE_SORT_VEC_FILE is alloc's vec/mod.rs, where Vec's spare capacity's slice is made. */
#define STD_STABLE_SORT_VEC_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "vec" STD_SEP "mod.rs"

/* STD_STABLE_SORT_MAX_FULL_ALLOC_BYTES is driftsort_main's most scratch it allocates for a whole slice: 8MB. */
#define STD_STABLE_SORT_MAX_FULL_ALLOC_BYTES ((size_t) 8000000)

/* STD_STABLE_SORT_STACK_BYTES is driftsort_main's AlignedStorage's size, its scratch on the stack. */
#define STD_STABLE_SORT_STACK_BYTES ((size_t) 4096)

/* STD_STABLE_SORT_MIN_SQRT_RUN_LEN is drift's shortest run it takes as good for short slices. */
#define STD_STABLE_SORT_MIN_SQRT_RUN_LEN ((size_t) 64)

static void STD_STABLE_SORT_NAME(driftsort_main) (STD_STABLE_SORT_T * v, size_t len, StdSortLess *is_less);
static void STD_STABLE_SORT_NAME(drift_sort) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
											  size_t scratch_len, bool eager_sort, StdSortLess *is_less);
static void STD_STABLE_SORT_NAME(stable_quicksort) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
													size_t scratch_len, StdSortLess *is_less);
static void STD_STABLE_SORT_NAME(quicksort) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
											 size_t scratch_len, uint32 limit,
											 const STD_STABLE_SORT_T * left_ancestor_pivot, StdSortLess *is_less);
static void STD_STABLE_SORT_NAME(merge) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
										 size_t scratch_len, size_t mid, StdSortLess *is_less);

/* small_sort_threshold is StableSmallSortTypeImpl's threshold: the general small sort's for a Freeze T, the insertion sort's otherwise. */
static inline size_t
STD_STABLE_SORT_NAME(small_sort_threshold) (void)
{
#if STD_STABLE_SORT_FREEZE
	return STD_SORT_SMALL_SORT_GENERAL_THRESHOLD;
#else
	return STD_SORT_SMALL_SORT_FALLBACK_THRESHOLD;
#endif
}

/* small_sort is StableSmallSortTypeImpl's small_sort, by the same choice as its threshold. */
static void
STD_STABLE_SORT_NAME(small_sort) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
								  size_t scratch_len, StdSortLess *is_less)
{
#if STD_STABLE_SORT_FREEZE
	STD_STABLE_SORT_SHARED(small_sort_general_with_scratch) (v, len, scratch, scratch_len, is_less);
#else
	(void) scratch;
	(void) scratch_len;
	if (len >= 2)
		STD_STABLE_SORT_SHARED(insertion_sort_shift_left) (v, len, 1, is_less);
#endif
}

/*
 * sort is core's stable sort: nothing for a zero-sized T or fewer than two
 * elements, the insertion sort for at most twenty, and driftsort otherwise.
 */
STD_STABLE_SORT_DEF void
STD_STABLE_SORT_NAME(sort) (STD_STABLE_SORT_T * v, size_t len, StdSortLess *is_less)
{
	if (STD_STABLE_SORT_SIZE == 0)
		return;
	if (len < 2)
		return;
	if (len <= STD_SORT_MAX_LEN_ALWAYS_INSERTION_SORT)
	{
		STD_STABLE_SORT_SHARED(insertion_sort_shift_left) (v, len, 1, is_less);
		return;
	}
	STD_STABLE_SORT_NAME(driftsort_main) (v, len, is_less);
}

/* STD_STABLE_SORT_DRIFTSORT_MAIN_FRAME_0_T is driftsort_main's state across its pad: its arguments and its heap scratch. */
#define STD_STABLE_SORT_DRIFTSORT_MAIN_FRAME_0_T STD_STABLE_SORT_NAME(driftsort_main_frame_0_t)
typedef struct STD_STABLE_SORT_DRIFTSORT_MAIN_FRAME_0_T
{
	STD_STABLE_SORT_T *v;
	size_t		len;
	StdSortLess *is_less;
	StdVec		heap_buf;
	bool		heap_buf_live;
}			STD_STABLE_SORT_DRIFTSORT_MAIN_FRAME_0_T;

/* driftsort_main_body_0 is driftsort_main's body: its scratch's length, the stack's AlignedStorage or the heap's buffer, and drift's sort. */
static void
STD_STABLE_SORT_NAME(driftsort_main_body_0) (void *frame_)
{
	STD_STABLE_SORT_DRIFTSORT_MAIN_FRAME_0_T *frame = frame_;
	size_t		max_full_alloc = STD_STABLE_SORT_MAX_FULL_ALLOC_BYTES / STD_STABLE_SORT_SIZE;
	size_t		len = frame->len;
	size_t		alloc_len = Max(Max(len - len / 2, Min(len, max_full_alloc)),
								STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN);
	union
	{
		STD_STABLE_SORT_T align_;
		uint8		storage[STD_STABLE_SORT_STACK_BYTES];
	}			stack_buf;
	STD_STABLE_SORT_T *scratch = (STD_STABLE_SORT_T *) stack_buf.storage;
	size_t		scratch_len = STD_STABLE_SORT_STACK_BYTES / STD_STABLE_SORT_SIZE;
	bool		eager_sort;

	std_slice_from_raw_parts_mut_check(scratch, scratch_len, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN,
									   STD_STABLE_SORT_MOD_FILE, 163, 18);
	if (!(scratch_len >= alloc_len))
	{
		std_vec_with_capacity_Checked(&frame->heap_buf, alloc_len, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
		frame->heap_buf_live = true;
		scratch = (STD_STABLE_SORT_T *) frame->heap_buf.ptr + frame->heap_buf.len;
		scratch_len = frame->heap_buf.cap - frame->heap_buf.len;
		std_slice_from_raw_parts_mut_check(scratch, scratch_len, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN,
										   STD_STABLE_SORT_VEC_FILE, 3305, 13);
	}
	eager_sort = len <= STD_STABLE_SORT_NAME(small_sort_threshold) () * 2;
	STD_STABLE_SORT_NAME(drift_sort) (frame->v, len, scratch, scratch_len, eager_sort, frame->is_less);
	if (frame->heap_buf_live)
	{
		frame->heap_buf_live = false;
		std_vec_free_Checked(&frame->heap_buf, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
	}
}

/* driftsort_main_pad_0 is driftsort_main's pad: the heap scratch's drop while it is live. */
static void
STD_STABLE_SORT_NAME(driftsort_main_pad_0) (void *frame_)
{
	STD_STABLE_SORT_DRIFTSORT_MAIN_FRAME_0_T *frame = frame_;

	if (frame->heap_buf_live)
		std_vec_free_Checked(&frame->heap_buf, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
}

/*
 * driftsort_main sorts a slice longer than the insertion sort's by drift,
 * with scratch of at least half its length, all of it up to 8MB's worth, and
 * the general small sort's, on the stack when that fits 4KB.
 */
static void
STD_STABLE_SORT_NAME(driftsort_main) (STD_STABLE_SORT_T * v, size_t len, StdSortLess *is_less)
{
	STD_STABLE_SORT_DRIFTSORT_MAIN_FRAME_0_T frame = {.v = v, .len = len, .is_less = is_less};

	std_landing(&frame, STD_STABLE_SORT_NAME(driftsort_main_body_0), STD_STABLE_SORT_NAME(driftsort_main_pad_0));
}

/*
 * DriftsortRun_new_sorted is DriftsortRun::new_sorted: a run of drift's is a
 * usize, its length shifted up by one, and whether it is sorted, the low bit.
 */
static inline size_t
STD_STABLE_SORT_NAME(DriftsortRun_new_sorted) (size_t length)
{
	return (length << 1) | 1;
}

/* DriftsortRun_new_unsorted is DriftsortRun::new_unsorted. */
static inline size_t
STD_STABLE_SORT_NAME(DriftsortRun_new_unsorted) (size_t length)
{
	return length << 1;
}

/* DriftsortRun_sorted is DriftsortRun::sorted. */
static inline bool
STD_STABLE_SORT_NAME(DriftsortRun_sorted) (size_t self)
{
	return (self & 1) == 1;
}

/* DriftsortRun_len is DriftsortRun::len. */
static inline size_t
STD_STABLE_SORT_NAME(DriftsortRun_len) (size_t self)
{
	return self >> 1;
}

/* merge_tree_scale_factor is the scale that maps a slice's positions onto 2^62, rounded up. */
static inline uint64
STD_STABLE_SORT_NAME(merge_tree_scale_factor) (size_t n)
{
	uint64		numerator = (uint64) 1 << 62;
	uint64		quotient = numerator / (uint64) n;

	return numerator % (uint64) n != 0 ? quotient + 1 : quotient;
}

/* merge_tree_depth is the depth in the merge tree of the boundary between two runs, by their scaled midpoints. */
static inline uint8
STD_STABLE_SORT_NAME(merge_tree_depth) (size_t left, size_t mid, size_t right, uint64 scale_factor)
{
	uint64		x = (uint64) left + (uint64) mid;
	uint64		y = (uint64) mid + (uint64) right;

	return (uint8) std_leading_zeros_u64((scale_factor * x) ^ (scale_factor * y));
}

/* sqrt_approx is drift's approximate square root, the shortest good run of a long slice. */
static size_t
STD_STABLE_SORT_NAME(sqrt_approx) (size_t n)
{
	uint32		ilog = std_ilog2_usize(n | 1, STD_STABLE_SORT_DRIFT_FILE, 184, 24);
	uint32		shift = ilog / 2 + ilog % 2;

	return (((size_t) 1 << shift) + (n >> shift)) / 2;
}

/*
 * logical_merge merges two runs: physically, both sorted first, when they
 * don't fit the scratch together or either is sorted, and otherwise only
 * logically, as one unsorted run.
 */
static size_t
STD_STABLE_SORT_NAME(logical_merge) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
									 size_t scratch_len, size_t left, size_t right, StdSortLess *is_less)
{
	bool		can_fit_in_scratch = len <= scratch_len;

	if (!can_fit_in_scratch || STD_STABLE_SORT_NAME(DriftsortRun_sorted) (left) ||
		STD_STABLE_SORT_NAME(DriftsortRun_sorted) (right))
	{
		size_t		left_len = STD_STABLE_SORT_NAME(DriftsortRun_len) (left);

		if (!STD_STABLE_SORT_NAME(DriftsortRun_sorted) (left))
		{
			std_range_to_check(left_len, len, STD_STABLE_SORT_DRIFT_FILE, 206, 36);
			STD_STABLE_SORT_NAME(stable_quicksort) (v, left_len, scratch, scratch_len, is_less);
		}
		if (!STD_STABLE_SORT_NAME(DriftsortRun_sorted) (right))
		{
			std_range_from_check(left_len, len, STD_STABLE_SORT_DRIFT_FILE, 209, 36);
			STD_STABLE_SORT_NAME(stable_quicksort) (v + left_len, len - left_len, scratch, scratch_len, is_less);
		}
		STD_STABLE_SORT_NAME(merge) (v, len, scratch, scratch_len, left_len, is_less);
		return STD_STABLE_SORT_NAME(DriftsortRun_new_sorted) (len);
	}
	return STD_STABLE_SORT_NAME(DriftsortRun_new_unsorted) (len);
}

/*
 * create_run makes the run at the start of a slice: the existing ascending
 * or reversed run when it is long enough, reversed into order, else an
 * eagerly sorted short run, else an unsorted one of the shortest good
 * length.
 */
static size_t
STD_STABLE_SORT_NAME(create_run) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
								  size_t scratch_len, size_t min_good_run_len, bool eager_sort,
								  StdSortLess *is_less)
{
	if (len >= min_good_run_len)
	{
		bool		was_reversed;
		size_t		run_len = STD_STABLE_SORT_SHARED(find_existing_run) (v, len, is_less, &was_reversed);

		if (run_len >= min_good_run_len)
		{
			if (was_reversed)
			{
				std_range_to_check(run_len, len, STD_STABLE_SORT_DRIFT_FILE, 242, 18);
				std_slice_reverse(v, run_len, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
			}
			return STD_STABLE_SORT_NAME(DriftsortRun_new_sorted) (run_len);
		}
	}
	if (eager_sort)
	{
		size_t		eager_run_len = Min(STD_STABLE_SORT_NAME(small_sort_threshold) (), len);

		std_range_to_check(eager_run_len, len, STD_STABLE_SORT_DRIFT_FILE, 255, 25);
		STD_STABLE_SORT_NAME(quicksort) (v, eager_run_len, scratch, scratch_len, 0, NULL, is_less);
		return STD_STABLE_SORT_NAME(DriftsortRun_new_sorted) (eager_run_len);
	}
	return STD_STABLE_SORT_NAME(DriftsortRun_new_unsorted) (Min(min_good_run_len, len));
}

/*
 * drift_sort is drift::sort: the slice's runs found or made left to right,
 * each pushed on the run stack after merging the runs above it whose merge
 * tree depth is at least the new boundary's, the last run sorted by the
 * stable quicksort when it is still unsorted.
 */
static void
STD_STABLE_SORT_NAME(drift_sort) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch, size_t scratch_len,
								  bool eager_sort, StdSortLess *is_less)
{
	uint64		scale_factor;
	size_t		min_good_run_len;
	size_t		stack_len = 0;
	size_t		runs[66];
	uint8		desired_depths[66];
	size_t		scan_idx = 0;
	size_t		prev_run = STD_STABLE_SORT_NAME(DriftsortRun_new_sorted) (0);

	if (len < 2)
		return;
	scale_factor = STD_STABLE_SORT_NAME(merge_tree_scale_factor) (len);
	if (len <= STD_STABLE_SORT_MIN_SQRT_RUN_LEN * STD_STABLE_SORT_MIN_SQRT_RUN_LEN)
		min_good_run_len = Min(len - len / 2, STD_STABLE_SORT_MIN_SQRT_RUN_LEN);
	else
		min_good_run_len = STD_STABLE_SORT_NAME(sqrt_approx) (len);
	for (;;)
	{
		size_t		next_run;
		uint8		desired_depth;

		if (scan_idx < len)
		{
			std_range_from_check(scan_idx, len, STD_STABLE_SORT_DRIFT_FILE, 64, 34);
			next_run = STD_STABLE_SORT_NAME(create_run) (v + scan_idx, len - scan_idx, scratch, scratch_len,
														 min_good_run_len, eager_sort, is_less);
			desired_depth = STD_STABLE_SORT_NAME(merge_tree_depth) (scan_idx - STD_STABLE_SORT_NAME(DriftsortRun_len) (prev_run),
																	scan_idx,
																	scan_idx + STD_STABLE_SORT_NAME(DriftsortRun_len) (next_run),
																	scale_factor);
		}
		else
		{
			next_run = STD_STABLE_SORT_NAME(DriftsortRun_new_sorted) (0);
			desired_depth = 0;
		}
		while (stack_len > 1 && desired_depths[stack_len - 1] >= desired_depth)
		{
			size_t		left = runs[stack_len - 1];
			size_t		merged_len = STD_STABLE_SORT_NAME(DriftsortRun_len) (left) +
				STD_STABLE_SORT_NAME(DriftsortRun_len) (prev_run);
			size_t		merge_start_idx = scan_idx - merged_len;

			std_slice_get_unchecked_mut_range_check(merge_start_idx, scan_idx, len, STD_STABLE_SORT_DRIFT_FILE, 93, 37);
			prev_run = STD_STABLE_SORT_NAME(logical_merge) (v + merge_start_idx, scan_idx - merge_start_idx, scratch,
															scratch_len, left, prev_run, is_less);
			stack_len--;
		}
		runs[stack_len] = prev_run;
		desired_depths[stack_len] = desired_depth;
		stack_len++;
		if (scan_idx >= len)
			break;
		scan_idx += STD_STABLE_SORT_NAME(DriftsortRun_len) (next_run);
		prev_run = next_run;
	}
	if (!STD_STABLE_SORT_NAME(DriftsortRun_sorted) (prev_run))
		STD_STABLE_SORT_NAME(stable_quicksort) (v, len, scratch, scratch_len, is_less);
}

/* stable_quicksort is the stable quicksort with a limit of 2 * floor(log2(len)) imbalanced partitions. */
static void
STD_STABLE_SORT_NAME(stable_quicksort) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
										size_t scratch_len, StdSortLess *is_less)
{
	uint32		limit = 2 * std_ilog2_usize(len | 1, STD_STABLE_SORT_DRIFT_FILE, 269, 35);

	STD_STABLE_SORT_NAME(quicksort) (v, len, scratch, scratch_len, limit, NULL, is_less);
}

/* PartitionState is stable_partition's state: where the left elements go, the scan, the count of left ones, and where the right ones go, backwards. */
#define STD_STABLE_SORT_PARTITION_STATE STD_STABLE_SORT_NAME(PartitionState)
typedef struct STD_STABLE_SORT_PARTITION_STATE
{
	STD_STABLE_SORT_T *scratch_base;
	const		STD_STABLE_SORT_T *scan;
	size_t		num_left;
	STD_STABLE_SORT_T *scratch_rev;
}			STD_STABLE_SORT_PARTITION_STATE;

/* PartitionState_partition_one copies the scanned element to the front of the scratch or to its back, by its side, and returns where it went. */
static inline STD_STABLE_SORT_T *
STD_STABLE_SORT_NAME(PartitionState_partition_one) (STD_STABLE_SORT_PARTITION_STATE * self, bool towards_left)
{
	STD_STABLE_SORT_T *dst_base;
	STD_STABLE_SORT_T *dst;

	self->scratch_rev = self->scratch_rev - 1;
	dst_base = towards_left ? self->scratch_base : self->scratch_rev;
	dst = dst_base + self->num_left;
	std_ptr_copy_nonoverlapping_Checked(self->scan, dst, 1, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
	self->num_left += (size_t) towards_left;
	self->scan = self->scan + 1;
	return dst;
}

/*
 * stable_partition partitions a slice stably around the element at
 * pivot_pos, the elements less than it (or not greater, for the equal
 * partition) first, through the scratch, with the pivot itself to the side
 * pivot_goes_left names, and returns the count on the left.
 */
static size_t
STD_STABLE_SORT_NAME(stable_partition) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch,
										size_t scratch_len, size_t pivot_pos, bool pivot_goes_left,
										StdSortLess *is_less)
{
	const		STD_STABLE_SORT_T *v_base = v;
	STD_STABLE_SORT_T *scratch_base = scratch;
	const		STD_STABLE_SORT_T *pivot;
	STD_STABLE_SORT_PARTITION_STATE state;
	STD_STABLE_SORT_T *pivot_in_scratch = NULL;
	size_t		loop_end_pos = pivot_pos;

	if (scratch_len < len || pivot_pos >= len)
		std_intrinsics_abort();
	pivot = v_base + pivot_pos;
	state.scratch_base = scratch_base;
	state.scan = v_base;
	state.num_left = 0;
	state.scratch_rev = scratch_base + len;
	for (;;)
	{
		const		STD_STABLE_SORT_T *loop_end;

		if (STD_STABLE_SORT_SIZE <= 16)
		{
			const		STD_STABLE_SORT_T *unroll_end = v_base + (loop_end_pos >= 3 ? loop_end_pos - 3 : 0);

			while (state.scan < unroll_end)
			{
				STD_STABLE_SORT_NAME(PartitionState_partition_one) (&state, STD_SORT_LESS(is_less, state.scan, pivot));
				STD_STABLE_SORT_NAME(PartitionState_partition_one) (&state, STD_SORT_LESS(is_less, state.scan, pivot));
				STD_STABLE_SORT_NAME(PartitionState_partition_one) (&state, STD_SORT_LESS(is_less, state.scan, pivot));
				STD_STABLE_SORT_NAME(PartitionState_partition_one) (&state, STD_SORT_LESS(is_less, state.scan, pivot));
			}
		}
		loop_end = v_base + loop_end_pos;
		while (state.scan < loop_end)
			STD_STABLE_SORT_NAME(PartitionState_partition_one) (&state, STD_SORT_LESS(is_less, state.scan, pivot));
		if (loop_end_pos == len)
			break;
		pivot_in_scratch = STD_STABLE_SORT_NAME(PartitionState_partition_one) (&state, pivot_goes_left);
		loop_end_pos = len;
	}
#if !STD_STABLE_SORT_FREEZE
	std_ptr_copy_nonoverlapping_Checked(pivot, pivot_in_scratch, 1, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
#else
	(void) pivot_in_scratch;
#endif
	std_ptr_copy_nonoverlapping_Checked(scratch_base, v, state.num_left, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
	for (size_t i = 0, i_end = len - state.num_left; i < i_end; i++)
		std_ptr_copy_nonoverlapping_Checked(scratch_base + (len - 1 - i), v + (state.num_left + i), 1,
											STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
	return state.num_left;
}

/* quicksort_closure_0 is quicksort's |a, b| !is_less(b, a), the equal partition's comparison. */
static bool
STD_STABLE_SORT_NAME(quicksort_closure_0) (void *captures, const void *a, const void *b)
{
	StdSortLess *is_less = captures;

	return !STD_SORT_LESS(is_less, b, a);
}

/*
 * quicksort is the stable quicksort: the small sort for a short slice,
 * drift's eager sort past the limit, and otherwise a stable partition around
 * the chosen pivot, an equal partition when the left ancestor pivot is no
 * less than it, the right part sorted recursively with the pivot as its
 * ancestor and the left one by the loop.
 */
static void
STD_STABLE_SORT_NAME(quicksort) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch, size_t scratch_len,
								 uint32 limit, const STD_STABLE_SORT_T * left_ancestor_pivot, StdSortLess *is_less)
{
	for (;;)
	{
		size_t		pivot_pos;
		STD_STABLE_SORT_T pivot_copy;
		const		STD_STABLE_SORT_T *pivot_ref;
		bool		perform_equal_partition = false;
		size_t		left_partition_len = 0;
		void	   *left;
		size_t		left_len;
		void	   *right;
		size_t		right_len;

		if (len <= STD_STABLE_SORT_NAME(small_sort_threshold) ())
		{
			STD_STABLE_SORT_NAME(small_sort) (v, len, scratch, scratch_len, is_less);
			return;
		}
		if (limit == 0)
		{
			STD_STABLE_SORT_NAME(drift_sort) (v, len, scratch, scratch_len, true, is_less);
			return;
		}
		limit--;
		pivot_pos = STD_STABLE_SORT_SHARED(choose_pivot) (v, len, is_less);
		std_index_check(pivot_pos, len, STD_STABLE_SORT_QUICKSORT_FILE, 45, 57);
		memcpy(&pivot_copy, &v[pivot_pos], STD_STABLE_SORT_SIZE);
#if STD_STABLE_SORT_FREEZE
		pivot_ref = &pivot_copy;
#else
		pivot_ref = NULL;
#endif
		if (left_ancestor_pivot != NULL)
		{
			std_index_check(pivot_pos, len, STD_STABLE_SORT_QUICKSORT_FILE, 60, 59);
			perform_equal_partition = !STD_SORT_LESS(is_less, left_ancestor_pivot, &v[pivot_pos]);
		}
		if (!perform_equal_partition)
		{
			left_partition_len = STD_STABLE_SORT_NAME(stable_partition) (v, len, scratch, scratch_len, pivot_pos,
																		 false, is_less);
			perform_equal_partition = left_partition_len == 0;
		}
		if (perform_equal_partition)
		{
			StdSortLess equal = {STD_STABLE_SORT_NAME(quicksort_closure_0), is_less};
			size_t		mid_eq = STD_STABLE_SORT_NAME(stable_partition) (v, len, scratch, scratch_len, pivot_pos,
																		 true, &equal);

			std_range_from_check(mid_eq, len, STD_STABLE_SORT_QUICKSORT_FILE, 71, 23);
			v = v + mid_eq;
			len = len - mid_eq;
			left_ancestor_pivot = NULL;
			continue;
		}
		std_slice_split_at_mut(v, len, left_partition_len, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN, &left,
							   &left_len, &right, &right_len, STD_STABLE_SORT_QUICKSORT_FILE, 77, 31);
		STD_STABLE_SORT_NAME(quicksort) (right, right_len, scratch, scratch_len, limit, pivot_ref, is_less);
		v = left;
		len = left_len;
	}
}

/* MergeState is merge's state: the part of the scratch not yet merged back, and where the merge writes next. */
#define STD_STABLE_SORT_MERGE_STATE STD_STABLE_SORT_NAME(MergeState)
typedef struct STD_STABLE_SORT_MERGE_STATE
{
	STD_STABLE_SORT_T *start;
	STD_STABLE_SORT_T *end;
	STD_STABLE_SORT_T *dst;
}			STD_STABLE_SORT_MERGE_STATE;

/* MergeState_merge_up merges the saved left run with the right run forwards, the left first among equals. */
static void
STD_STABLE_SORT_NAME(MergeState_merge_up) (STD_STABLE_SORT_MERGE_STATE * self, const STD_STABLE_SORT_T * right,
										   const STD_STABLE_SORT_T * right_end, StdSortLess *is_less)
{
	while (self->start != self->end && right != right_end)
	{
		bool		consume_left = !STD_SORT_LESS(is_less, right, self->start);
		const		STD_STABLE_SORT_T *src = consume_left ? self->start : right;

		std_ptr_copy_nonoverlapping_Checked(src, self->dst, 1, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
		self->start = self->start + (size_t) consume_left;
		right = right + (size_t) !consume_left;
		self->dst = self->dst + 1;
	}
}

/* MergeState_merge_down merges the left run with the saved right run backwards, the right last among equals. */
static void
STD_STABLE_SORT_NAME(MergeState_merge_down) (STD_STABLE_SORT_MERGE_STATE * self,
											 const STD_STABLE_SORT_T * left_end, const STD_STABLE_SORT_T * right_end,
											 STD_STABLE_SORT_T * out, StdSortLess *is_less)
{
	for (;;)
	{
		STD_STABLE_SORT_T *left = self->dst - 1;
		STD_STABLE_SORT_T *right = self->end - 1;
		bool		consume_left;
		STD_STABLE_SORT_T *src;

		out = out - 1;
		consume_left = STD_SORT_LESS(is_less, right, left);
		src = consume_left ? left : right;
		std_ptr_copy_nonoverlapping_Checked(src, out, 1, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
		self->dst = left + (size_t) !consume_left;
		self->end = right + (size_t) consume_left;
		if (self->dst == left_end || self->end == right_end)
			break;
	}
}

/* MergeState_drop is MergeState's drop: the rest of the saved run copied to where the merge writes next. */
static void
STD_STABLE_SORT_NAME(MergeState_drop) (STD_STABLE_SORT_MERGE_STATE * self)
{
	size_t		len = std_ptr_offset_from_unsigned_Checked(self->end, self->start, STD_STABLE_SORT_SIZE,
														   STD_STABLE_SORT_MERGE_FILE, 146, 32);

	std_ptr_copy_nonoverlapping_Checked(self->start, self->dst, len, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
}

/* STD_STABLE_SORT_MERGE_FRAME_0_T is merge's state across its pad: its runs' bounds and its MergeState. */
#define STD_STABLE_SORT_MERGE_FRAME_0_T STD_STABLE_SORT_NAME(merge_frame_0_t)
typedef struct STD_STABLE_SORT_MERGE_FRAME_0_T
{
	STD_STABLE_SORT_T *v_base;
	STD_STABLE_SORT_T *v_mid;
	STD_STABLE_SORT_T *v_end;
	STD_STABLE_SORT_T *buf;
	bool		left_is_shorter;
	StdSortLess *is_less;
	STD_STABLE_SORT_MERGE_STATE merge_state;
}			STD_STABLE_SORT_MERGE_FRAME_0_T;

/* merge_body_0 is merge's merge: up from the saved left run, or down from the saved right one. */
static void
STD_STABLE_SORT_NAME(merge_body_0) (void *frame_)
{
	STD_STABLE_SORT_MERGE_FRAME_0_T *frame = frame_;

	if (frame->left_is_shorter)
		STD_STABLE_SORT_NAME(MergeState_merge_up) (&frame->merge_state, frame->v_mid, frame->v_end, frame->is_less);
	else
		STD_STABLE_SORT_NAME(MergeState_merge_down) (&frame->merge_state, frame->v_base, frame->buf, frame->v_end,
													 frame->is_less);
	STD_STABLE_SORT_NAME(MergeState_drop) (&frame->merge_state);
}

/* merge_pad_0 is merge's pad: MergeState's drop, which puts the saved run's rest back should is_less panic. */
static void
STD_STABLE_SORT_NAME(merge_pad_0) (void *frame_)
{
	STD_STABLE_SORT_MERGE_FRAME_0_T *frame = frame_;

	STD_STABLE_SORT_NAME(MergeState_drop) (&frame->merge_state);
}

/*
 * merge merges the sorted runs v[..mid] and v[mid..] stably, the shorter one
 * saved in the scratch first, and does nothing when either is empty or the
 * scratch can't hold the shorter.
 */
static void
STD_STABLE_SORT_NAME(merge) (STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_T * scratch, size_t scratch_len,
							 size_t mid, StdSortLess *is_less)
{
	STD_STABLE_SORT_MERGE_FRAME_0_T frame;
	STD_STABLE_SORT_T *save_base;
	size_t		save_len;
	size_t		left_len;
	size_t		right_len;

	if (mid == 0 || mid >= len || scratch_len < Min(mid, len - mid))
		return;
	frame.buf = scratch;
	frame.v_base = v;
	frame.v_mid = v + mid;
	frame.v_end = v + len;
	left_len = mid;
	right_len = len - mid;
	frame.left_is_shorter = left_len <= right_len;
	save_base = frame.left_is_shorter ? frame.v_base : frame.v_mid;
	save_len = frame.left_is_shorter ? left_len : right_len;
	std_ptr_copy_nonoverlapping_Checked(save_base, frame.buf, save_len, STD_STABLE_SORT_SIZE, STD_STABLE_SORT_ALIGN);
	frame.merge_state.start = frame.buf;
	frame.merge_state.end = frame.buf + save_len;
	frame.merge_state.dst = save_base;
	frame.is_less = is_less;
	std_landing(&frame, STD_STABLE_SORT_NAME(merge_body_0), STD_STABLE_SORT_NAME(merge_pad_0));
}

#ifndef STD_STABLE_SORT_WITHOUT_SORT

/* slice_sort is <[T]>::sort: core's stable sort with T's PartialOrd::lt. */
STD_STABLE_SORT_DEF void
STD_STABLE_SORT_SLICE(STD_STABLE_SORT_T * v, size_t len)
{
	StdSortLess is_less = {STD_STABLE_SORT_LT, NULL};

	STD_STABLE_SORT_NAME(sort) (v, len, &is_less);
}

#endif							/* STD_STABLE_SORT_WITHOUT_SORT */

#ifdef STD_STABLE_SORT_WITH_BY

/* sort_by_closure_0 is sort_by's |a, b| compare(a, b) == Less, the stable sort's is_less. */
static bool
STD_STABLE_SORT_NAME(sort_by_closure_0) (void *captures, const void *a, const void *b)
{
	StdSortCompare *compare = captures;

	return compare->f(compare->captures, a, b) == STD_ORDERING_LESS;
}

/* STD_STABLE_SORT_SORT_BY_FRAME_0_T is sort_by's state across its pad: the slice, compare's captures, drop, and liveness, and is_less. */
#define STD_STABLE_SORT_SORT_BY_FRAME_0_T STD_STABLE_SORT_NAME(sort_by_frame_0_t)
typedef struct STD_STABLE_SORT_SORT_BY_FRAME_0_T
{
	STD_STABLE_SORT_T *v;
	size_t		len;
	void	   *compare_captures;
	void		(*compare_drop) (void *);
	bool		compare_live;
	StdSortLess is_less;
}			STD_STABLE_SORT_SORT_BY_FRAME_0_T;

/* sort_by_body_0 is sort_by's stable sort, then compare's drop. */
static void
STD_STABLE_SORT_NAME(sort_by_body_0) (void *frame_)
{
	STD_STABLE_SORT_SORT_BY_FRAME_0_T *frame = frame_;

	STD_STABLE_SORT_NAME(sort) (frame->v, frame->len, &frame->is_less);
	frame->compare_live = false;
	if (frame->compare_drop != NULL)
		frame->compare_drop(frame->compare_captures);
}

/* sort_by_pad_0 is sort_by's pad: compare's drop while it is live. */
static void
STD_STABLE_SORT_NAME(sort_by_pad_0) (void *frame_)
{
	STD_STABLE_SORT_SORT_BY_FRAME_0_T *frame = frame_;

	if (frame->compare_live && frame->compare_drop != NULL)
		frame->compare_drop(frame->compare_captures);
}

/*
 * slice_sort_by is <[T]>::sort_by: core's stable sort, one element before
 * another where compare orders it Less. It takes compare and drops it at the
 * end, or should the sort panic.
 */
STD_STABLE_SORT_DEF void
STD_STABLE_SORT_SLICE_BY(STD_STABLE_SORT_T * v, size_t len, StdOrdering (*compare) (void *, const void *, const void *),
						 void *compare_captures, void (*compare_drop) (void *))
{
	StdSortCompare by = {compare, compare_captures};
	STD_STABLE_SORT_SORT_BY_FRAME_0_T frame = {.v = v, .len = len, .compare_captures = compare_captures,
	.compare_drop = compare_drop, .compare_live = true, .is_less = {STD_STABLE_SORT_NAME(sort_by_closure_0), &by}};

	std_landing(&frame, STD_STABLE_SORT_NAME(sort_by_body_0), STD_STABLE_SORT_NAME(sort_by_pad_0));
}

#undef STD_STABLE_SORT_SORT_BY_FRAME_0_T

#endif							/* STD_STABLE_SORT_WITH_BY */

#ifdef STD_STABLE_SORT_WITH_BY_KEY

/* STD_STABLE_SORT_BY_KEY_NAME names sort_by_key's functions at T and K. */
#define STD_STABLE_SORT_BY_KEY_NAME(name) \
	STD_STABLE_SORT_CAT(STD_STABLE_SORT_CAT(STD_STABLE_SORT_NAME(name), _), STD_STABLE_SORT_KEY_KS)

/* STD_STABLE_SORT_BY_KEY_F is sort_by_key's f at K: the closure's function and its captures. */
#define STD_STABLE_SORT_BY_KEY_F STD_STABLE_SORT_BY_KEY_NAME(sort_by_key_f_t)
typedef struct STD_STABLE_SORT_BY_KEY_F
{
	STD_STABLE_SORT_KEY_K(*f) (void *captures, const void *elem);
	void	   *captures;
}			STD_STABLE_SORT_BY_KEY_F;

/* sort_by_key_closure_0 is sort_by_key's |a, b| f(a).lt(&f(b)), the stable sort's is_less. */
static bool
STD_STABLE_SORT_BY_KEY_NAME(sort_by_key_closure_0) (void *captures, const void *a, const void *b)
{
	STD_STABLE_SORT_BY_KEY_F *f = captures;
	STD_STABLE_SORT_KEY_K key_a = f->f(f->captures, a);
	STD_STABLE_SORT_KEY_K key_b = f->f(f->captures, b);

	return STD_STABLE_SORT_KEY_LT(&key_a, &key_b);
}

/* STD_STABLE_SORT_SORT_BY_KEY_FRAME_0_T is sort_by_key's state across its pad: the slice, f's captures, drop, and liveness, and is_less. */
#define STD_STABLE_SORT_SORT_BY_KEY_FRAME_0_T STD_STABLE_SORT_BY_KEY_NAME(sort_by_key_frame_0_t)
typedef struct STD_STABLE_SORT_SORT_BY_KEY_FRAME_0_T
{
	STD_STABLE_SORT_T *v;
	size_t		len;
	void	   *f_captures;
	void		(*f_drop) (void *);
	bool		f_live;
	StdSortLess is_less;
}			STD_STABLE_SORT_SORT_BY_KEY_FRAME_0_T;

/* sort_by_key_body_0 is sort_by_key's stable sort, then f's drop. */
static void
STD_STABLE_SORT_BY_KEY_NAME(sort_by_key_body_0) (void *frame_)
{
	STD_STABLE_SORT_SORT_BY_KEY_FRAME_0_T *frame = frame_;

	STD_STABLE_SORT_NAME(sort) (frame->v, frame->len, &frame->is_less);
	frame->f_live = false;
	if (frame->f_drop != NULL)
		frame->f_drop(frame->f_captures);
}

/* sort_by_key_pad_0 is sort_by_key's pad: f's drop while it is live. */
static void
STD_STABLE_SORT_BY_KEY_NAME(sort_by_key_pad_0) (void *frame_)
{
	STD_STABLE_SORT_SORT_BY_KEY_FRAME_0_T *frame = frame_;

	if (frame->f_live && frame->f_drop != NULL)
		frame->f_drop(frame->f_captures);
}

/*
 * slice_sort_by_key is <[T]>::sort_by_key: core's stable sort, one element
 * before another where f's key of it is less than f's key of the other. It
 * takes f and drops it at the end, or should the sort panic.
 */
STD_STABLE_SORT_DEF void
STD_STABLE_SORT_SLICE_BY_KEY(STD_STABLE_SORT_T * v, size_t len, STD_STABLE_SORT_KEY_K(*f) (void *, const void *),
							 void *f_captures, void (*f_drop) (void *))
{
	STD_STABLE_SORT_BY_KEY_F by = {f, f_captures};
	STD_STABLE_SORT_SORT_BY_KEY_FRAME_0_T frame = {.v = v, .len = len, .f_captures = f_captures, .f_drop = f_drop,
	.f_live = true, .is_less = {STD_STABLE_SORT_BY_KEY_NAME(sort_by_key_closure_0), &by}};

	std_landing(&frame, STD_STABLE_SORT_BY_KEY_NAME(sort_by_key_body_0), STD_STABLE_SORT_BY_KEY_NAME(sort_by_key_pad_0));
}

#undef STD_STABLE_SORT_SORT_BY_KEY_FRAME_0_T
#undef STD_STABLE_SORT_BY_KEY_F
#undef STD_STABLE_SORT_BY_KEY_NAME

#endif							/* STD_STABLE_SORT_WITH_BY_KEY */

#undef STD_STABLE_SORT_SIZE
#undef STD_STABLE_SORT_ALIGN
#undef STD_STABLE_SORT_DIR
#undef STD_STABLE_SORT_DRIFT_FILE
#undef STD_STABLE_SORT_QUICKSORT_FILE
#undef STD_STABLE_SORT_MERGE_FILE
#undef STD_STABLE_SORT_MOD_FILE
#undef STD_STABLE_SORT_VEC_FILE
#undef STD_STABLE_SORT_MAX_FULL_ALLOC_BYTES
#undef STD_STABLE_SORT_STACK_BYTES
#undef STD_STABLE_SORT_MIN_SQRT_RUN_LEN
#undef STD_STABLE_SORT_DRIFTSORT_MAIN_FRAME_0_T
#undef STD_STABLE_SORT_PARTITION_STATE
#undef STD_STABLE_SORT_MERGE_STATE
#undef STD_STABLE_SORT_MERGE_FRAME_0_T

#endif							/* STD_STABLE_SORT_DEFINE */

#endif							/* STD_STABLE_SORT_UNIT */

#undef STD_STABLE_SORT_CAT_
#undef STD_STABLE_SORT_CAT
#undef STD_STABLE_SORT_DEF_static
#undef STD_STABLE_SORT_DEF_extern
#undef STD_STABLE_SORT_DEF
#undef STD_STABLE_SORT_NAME
#undef STD_STABLE_SORT_SHARED
#undef STD_STABLE_SORT_SLICE
#undef STD_STABLE_SORT_SLICE_BY
#undef STD_STABLE_SORT_WITH_BY
#undef STD_STABLE_SORT_SLICE_BY_KEY
#undef STD_STABLE_SORT_WITH_BY_KEY
#undef STD_STABLE_SORT_KEY_KS
#undef STD_STABLE_SORT_KEY_K
#undef STD_STABLE_SORT_KEY_LT
#undef STD_STABLE_SORT_WITHOUT_SORT
#undef STD_STABLE_SORT_UNIT
#undef STD_STABLE_SORT_SCOPE
#undef STD_STABLE_SORT_DECLARE
#undef STD_STABLE_SORT_DEFINE
#undef STD_STABLE_SORT_TS
#undef STD_STABLE_SORT_T
#undef STD_STABLE_SORT_FREEZE
#undef STD_STABLE_SORT_LT
