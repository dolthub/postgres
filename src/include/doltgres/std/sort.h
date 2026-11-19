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
 * The C forms of slices' sorts (the port text's sections 2 and 9): what
 * sort_template.h's instances share, the comparator's form, core's
 * constants, the Rust files their panics and checks name, and the one
 * function of the sort that isn't generic. An instance comes from
 * sort_template.h.
 */
#ifndef DOLTGRES_STD_SORT_H
#define DOLTGRES_STD_SORT_H

#include "doltgres/std/cmp.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* StdIsLess is a sort's is_less: whether one element orders before another, with the closure's captures. */
typedef bool (*StdIsLess) (void *captures, const void *a, const void *b);

/* StdSortLess is a sort's is_less: its function and its captures, which the sort passes as &mut F. */
typedef struct StdSortLess
{
	StdIsLess	f;
	void	   *captures;
} StdSortLess;

/* STD_SORT_LESS calls a sort's is_less on two elements. */
#define STD_SORT_LESS(is_less, a, b) ((is_less)->f((is_less)->captures, (a), (b)))

/* STD_SORT_SMALL_SORT_FALLBACK_THRESHOLD is the largest slice the insertion sort fallback takes. */
#define STD_SORT_SMALL_SORT_FALLBACK_THRESHOLD 16

/* STD_SORT_SMALL_SORT_GENERAL_THRESHOLD is the largest slice the general small sort takes. */
#define STD_SORT_SMALL_SORT_GENERAL_THRESHOLD 32

/* STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN is the general small sort's scratch, in elements. */
#define STD_SORT_SMALL_SORT_GENERAL_SCRATCH_LEN (STD_SORT_SMALL_SORT_GENERAL_THRESHOLD + 16)

/* STD_SORT_SMALL_SORT_NETWORK_THRESHOLD is the largest slice the sorting network small sort takes. */
#define STD_SORT_SMALL_SORT_NETWORK_THRESHOLD 32

/* STD_SORT_SMALL_SORT_NETWORK_SCRATCH_LEN is the network small sort's scratch, in elements. */
#define STD_SORT_SMALL_SORT_NETWORK_SCRATCH_LEN STD_SORT_SMALL_SORT_NETWORK_THRESHOLD

/* STD_SORT_MAX_STACK_ARRAY_SIZE is the most bytes a small sort's scratch may take on the stack. */
#define STD_SORT_MAX_STACK_ARRAY_SIZE 4096

/* STD_SORT_PSEUDO_MEDIAN_REC_THRESHOLD is the length from which the pivot is a recursive pseudomedian. */
#define STD_SORT_PSEUDO_MEDIAN_REC_THRESHOLD 64

/* STD_SORT_MAX_LEN_ALWAYS_INSERTION_SORT is the longest slice sort sorts by insertion alone. */
#define STD_SORT_MAX_LEN_ALWAYS_INSERTION_SORT 20

/* STD_SORT_MAX_BRANCHLESS_PARTITION_SIZE is the largest element the branchless partition takes. */
#define STD_SORT_MAX_BRANCHLESS_PARTITION_SIZE 96

/* The sort's Rust files, which its panics and checks name. */
#define STD_SORT_DIR STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "slice" STD_SEP "sort" STD_SEP
#define STD_SORT_SHARED_FILE STD_SORT_DIR "shared" STD_SEP "mod.rs"
#define STD_SORT_PIVOT_FILE STD_SORT_DIR "shared" STD_SEP "pivot.rs"
#define STD_SORT_SMALLSORT_FILE STD_SORT_DIR "shared" STD_SEP "smallsort.rs"
#define STD_SORT_UNSTABLE_FILE STD_SORT_DIR "unstable" STD_SEP "mod.rs"
#define STD_SORT_HEAPSORT_FILE STD_SORT_DIR "unstable" STD_SEP "heapsort.rs"
#define STD_SORT_QUICKSORT_FILE STD_SORT_DIR "unstable" STD_SEP "quicksort.rs"
#define STD_SORT_SELECT_FILE STD_SORT_DIR "select.rs"

/* StdSortCompare is sort_unstable_by's compare: the closure's function and its captures. */
typedef struct StdSortCompare
{
	StdOrdering (*f) (void *captures, const void *a, const void *b);
	void	   *captures;
} StdSortCompare;

pg_noreturn extern void std_sort_panic_on_ord_violation(void);
extern bool std_slice_sort_unstable_by_is_less(void *captures, const void *a, const void *b);

#ifdef DOLTGRES_TEST_HOST
extern void test_std_sort_tests_a_wide_copy_element_takes_the_fallback_small_sort(void);
#endif

#endif							/* DOLTGRES_STD_SORT_H */
