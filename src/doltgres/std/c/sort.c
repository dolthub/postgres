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
 * The one function of slices' sorts that isn't generic (the port text's
 * section 9), ported from core/src/slice/sort/shared/smallsort.rs at Rust
 * 1.99.1; the generic rest is sort_template.h's.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/sort.h"

/* std_sort_panic_on_ord_violation is the panic of a comparator that isn't a total order. */
void
std_sort_panic_on_ord_violation(void)
{
	static const char text[] = "user-provided comparison function does not correctly implement a total order";

	std_panic(STD_SORT_SMALLSORT_FILE, 854, 5, text, sizeof(text) - 1);
}

/*
 * std_slice_sort_unstable_by_is_less is sort_unstable_by's is_less, the
 * closure it hands the sort: whether compare orders one element Less than
 * another.
 */
bool
std_slice_sort_unstable_by_is_less(void *captures, const void *a, const void *b)
{
	StdSortCompare *compare = captures;

	return compare->f(compare->captures, a, b) == STD_ORDERING_LESS;
}

#ifdef DOLTGRES_TEST_HOST

/* StdSortTestsWide is a 96-byte Copy and Freeze element: a key, an id, and padding. */
typedef struct StdSortTestsWide
{
	uint64		key;
	uint64		id;
	uint8		pad[80];
} StdSortTestsWide;

/* STD_SORT_TESTS_WIDE_COMPARES is how many comparisons Rust's sort of the test's elements makes. */
#define STD_SORT_TESTS_WIDE_COMPARES 94

/* StdSortTestsWideSeen is what the test's PartialOrd::lt records: the ids of each pair it compared, in order. */
typedef struct StdSortTestsWideSeen
{
	size_t		len;
	uint64		pairs[STD_SORT_TESTS_WIDE_COMPARES + 1][2];
} StdSortTestsWideSeen;

/* std_sort_tests_WIDE_SEEN is the pairs the test's PartialOrd::lt has compared. */
static StdSortTestsWideSeen std_sort_tests_WIDE_SEEN;

/* std_sort_tests_Wide_PartialOrd_lt is the test element's PartialOrd::lt: the pair's ids recorded, then its keys' <. */
static bool
std_sort_tests_Wide_PartialOrd_lt(const StdSortTestsWide *self, const StdSortTestsWide *other)
{
	StdSortTestsWideSeen *seen = &std_sort_tests_WIDE_SEEN;

	if (seen->len < lengthof(seen->pairs))
	{
		seen->pairs[seen->len][0] = self->id;
		seen->pairs[seen->len][1] = other->id;
	}
	seen->len++;
	return self->key < other->key;
}

/*
 * std_sort_tests_Wide_PartialOrd_lt_FnMut_call_mut is the fn item the test
 * element's PartialOrd::lt's FnMut shim, as the sort passes it for is_less.
 */
static bool
std_sort_tests_Wide_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_sort_tests_Wide_PartialOrd_lt(a, b);
}

/* sort_unstable at the 96-byte element, past the network's and the general small sort's sizes. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS Wide
#define STD_SORT_T StdSortTestsWide
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT std_sort_tests_Wide_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/sort_template.h"

/*
 * test_std_sort_tests_a_wide_copy_element_takes_the_fallback_small_sort
 * sorts 24 of the 96-byte elements by keys with many equals, and checks
 * their order and every pair its PartialOrd::lt compared against Rust
 * 1.99.1's sort_unstable of the same elements, whose small sort is the
 * fallback, insertion below 16.
 */
void
test_std_sort_tests_a_wide_copy_element_takes_the_fallback_small_sort(void)
{
	static const uint64 keys[24] = {3, 0, 2, 4, 1, 3, 0, 2, 4, 1, 3, 0, 2, 4, 1, 3, 0, 2, 4, 1, 3, 0, 2, 4};
	static const uint64 order[24] = {
		1, 6, 11, 16, 21, 4, 9, 14, 19, 12, 7, 17, 2, 22, 10, 5, 0, 15, 20, 13, 3, 18, 8, 23
	};
	static const uint64 pairs[STD_SORT_TESTS_WIDE_COMPARES][2] = {
		{1, 0}, {2, 1}, {0, 12}, {0, 21}, {12, 21}, {2, 12}, {3, 12}, {4, 12}, {5, 12}, {6, 12}, {7, 12},
		{8, 12}, {9, 12}, {10, 12}, {11, 12}, {0, 12}, {13, 12}, {14, 12}, {15, 12}, {16, 12}, {17, 12},
		{18, 12}, {19, 12}, {20, 12}, {21, 12}, {22, 12}, {23, 12}, {1, 12}, {4, 1}, {6, 4}, {6, 1},
		{9, 4}, {11, 9}, {11, 4}, {11, 6}, {14, 9}, {16, 14}, {16, 9}, {16, 4}, {16, 11}, {19, 14},
		{21, 19}, {21, 14}, {21, 9}, {21, 4}, {21, 16}, {5, 10}, {0, 5}, {13, 0}, {3, 13}, {15, 3},
		{15, 13}, {15, 0}, {7, 3}, {7, 13}, {7, 15}, {7, 0}, {7, 5}, {7, 10}, {17, 3}, {17, 13},
		{17, 15}, {17, 0}, {17, 5}, {17, 10}, {17, 7}, {18, 3}, {8, 18}, {20, 8}, {20, 18}, {20, 3},
		{20, 13}, {20, 15}, {2, 8}, {2, 18}, {2, 3}, {2, 13}, {2, 20}, {2, 15}, {2, 0}, {2, 5}, {2, 10},
		{2, 17}, {22, 8}, {22, 18}, {22, 3}, {22, 13}, {22, 20}, {22, 15}, {22, 0}, {22, 5}, {22, 10},
		{22, 2}, {23, 8}
	};
	StdSortTestsWide v[24] = {0};
	StdSortTestsWideSeen *seen = &std_sort_tests_WIDE_SEEN;

	for (int i = 0; i < 24; i++)
	{
		v[i].key = keys[i];
		v[i].id = (uint64) i;
	}
	memset(seen, 0, sizeof(*seen));
	std_slice_sort_unstable_Wide(v, 24);
	if (seen->len != STD_SORT_TESTS_WIDE_COMPARES)
		std_panic(__FILE__, __LINE__, 0, "the sort made other than Rust's comparisons",
				  sizeof("the sort made other than Rust's comparisons") - 1);
	for (int i = 0; i < STD_SORT_TESTS_WIDE_COMPARES; i++)
		if (seen->pairs[i][0] != pairs[i][0] || seen->pairs[i][1] != pairs[i][1])
			std_panic(__FILE__, __LINE__, 0, "the sort compared a pair Rust's doesn't",
					  sizeof("the sort compared a pair Rust's doesn't") - 1);
	for (int i = 0; i < 24; i++)
		if (v[i].id != order[i])
			std_panic(__FILE__, __LINE__, 0, "the elements are not in Rust's order",
					  sizeof("the elements are not in Rust's order") - 1);
}

#endif							/* DOLTGRES_TEST_HOST */
