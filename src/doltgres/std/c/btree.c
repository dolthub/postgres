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
 * The functions of Rust's B-tree that take no key or value (the port text's
 * section 9), from alloc/src/collections/btree/node.rs at Rust 1.99.1:
 * splitpoint and move_to_slice, which every instance of btree_template.h
 * calls.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/btree.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"

/*
 * std_btree_splitpoint is splitpoint: the key and value a full node splits
 * around for an insertion at an edge, and the edge the insertion takes in
 * the left or the right half.
 */
void
std_btree_splitpoint(size_t edge_idx, size_t *out_0, StdBTreeLeftOrRight *out_1)
{
	if (edge_idx < STD_BTREE_EDGE_IDX_LEFT_OF_CENTER)
	{
		*out_0 = STD_BTREE_KV_IDX_CENTER - 1;
		out_1->tag = STD_BTREE_LEFT_OR_RIGHT_LEFT;
		out_1->u.left = edge_idx;
	}
	else if (edge_idx == STD_BTREE_EDGE_IDX_LEFT_OF_CENTER)
	{
		*out_0 = STD_BTREE_KV_IDX_CENTER;
		out_1->tag = STD_BTREE_LEFT_OR_RIGHT_LEFT;
		out_1->u.left = edge_idx;
	}
	else if (edge_idx == STD_BTREE_EDGE_IDX_RIGHT_OF_CENTER)
	{
		*out_0 = STD_BTREE_KV_IDX_CENTER;
		out_1->tag = STD_BTREE_LEFT_OR_RIGHT_RIGHT;
		out_1->u.right = 0;
	}
	else
	{
		*out_0 = STD_BTREE_KV_IDX_CENTER + 1;
		out_1->tag = STD_BTREE_LEFT_OR_RIGHT_RIGHT;
		out_1->u.right = edge_idx - (STD_BTREE_KV_IDX_CENTER + 1 + 1);
	}
}

/*
 * std_btree_move_to_slice is move_to_slice: a slice's elements of a size and
 * alignment moved into another of the same length.
 */
void
std_btree_move_to_slice(void *src, size_t src_len, void *dst, size_t dst_len, size_t size, size_t align)
{
	if (!(src_len == dst_len))
		std_panic(STD_BTREE_NODE_FILE, 1876, 5, "assertion failed: src.len() == dst.len()",
				  sizeof("assertion failed: src.len() == dst.len()") - 1);
	std_ptr_copy_nonoverlapping_Checked(src, dst, src_len, size, align);
}

#ifdef DOLTGRES_TEST_HOST

/* std_btree_tests_Aligned64_Ord_cmp is Aligned64's derived Ord: its bytes compared in order. */
static StdOrdering
std_btree_tests_Aligned64_Ord_cmp(const void *a, const void *b)
{
	int			c = memcmp(((const StdTestsAligned64 *) a)->bytes, ((const StdTestsAligned64 *) b)->bytes, 64);

	return c < 0 ? STD_ORDERING_LESS : (c > 0 ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* BTreeMap<Aligned64, u64>, the over-aligned test map. */
#define STD_BTREE_SCOPE static
#define STD_BTREE_DECLARE
#define STD_BTREE_DEFINE
#define STD_BTREE_KS Aligned64
#define STD_BTREE_K StdTestsAligned64
#define STD_BTREE_VS U64
#define STD_BTREE_V uint64
#define STD_BTREE_LAYOUT A64_U64
#define STD_BTREE_CMP(a, b) std_btree_tests_Aligned64_Ord_cmp((a), (b))
#define STD_BTREE_KEY_DROP(k) ((void) (k))
#define STD_BTREE_VAL_DROP(v) ((void) (v))
#define STD_BTREE_VAL_DEFAULT(v) (*(v) = 0)
#define STD_BTREE_TABLES
#define STD_BTREE_TABLE_ARGS
#include "doltgres/std/btree_template.h"

/*
 * test_std_btree_tests_an_over_aligned_key_stays_aligned checks a map of
 * 64-byte-aligned keys filled past a split: its leaves and its internal root
 * are at 64, each key is found with its value, and the nodes are freed by
 * their layouts.
 */
void
test_std_btree_tests_an_over_aligned_key_stays_aligned(void)
{
	StdBTreeMap map = STD_BTREE_MAP_NEW;
	StdBTreeLeafNode **edges;

	for (int i = 0; i < 2 * STD_BTREE_CAPACITY; i++)
	{
		StdTestsAligned64 key;
		StdBTreeEntryAligned64U64 entry;

		memset(key.bytes, i, sizeof(key.bytes));
		entry = std_btree_BTreeMapAligned64U64_entry(&map, &key);
		*std_btree_EntryAligned64U64_or_default(&entry) = (uint64) i;
	}
	if (map.root.height != 1 || map.length != 2 * STD_BTREE_CAPACITY)
		std_panic(__FILE__, __LINE__, 0, "the map did not split once", sizeof("the map did not split once") - 1);
	if ((uintptr_t) map.root.node % 64 != 0)
		std_panic(__FILE__, __LINE__, 0, "the internal root is not aligned to 64",
				  sizeof("the internal root is not aligned to 64") - 1);
	edges = (StdBTreeLeafNode **) ((char *) map.root.node + STD_BTREE_LAYOUT_A64_U64_EDGES);
	for (int e = 0; e <= *(uint16 *) ((char *) map.root.node + STD_BTREE_LAYOUT_A64_U64_LEN); e++)
		if ((uintptr_t) edges[e] % 64 != 0)
			std_panic(__FILE__, __LINE__, 0, "a leaf is not aligned to 64", sizeof("a leaf is not aligned to 64") - 1);
	for (int i = 0; i < 2 * STD_BTREE_CAPACITY; i++)
	{
		StdTestsAligned64 key;
		uint64 const *at;

		memset(key.bytes, i, sizeof(key.bytes));
		at = std_btree_BTreeMapAligned64U64_get(&map, &key);
		if (at == NULL || *at != (uint64) i)
			std_panic(__FILE__, __LINE__, 0, "a key's value was not found", sizeof("a key's value was not found") - 1);
	}
	for (int e = 0; e <= *(uint16 *) ((char *) map.root.node + STD_BTREE_LAYOUT_A64_U64_LEN); e++)
		std_free(edges[e], STD_BTREE_LAYOUT_A64_U64_LEAF_SIZE, STD_BTREE_LAYOUT_A64_U64_LEAF_ALIGN);
	std_free(map.root.node, STD_BTREE_LAYOUT_A64_U64_INTERNAL_SIZE, STD_BTREE_LAYOUT_A64_U64_INTERNAL_ALIGN);
}

/* StdTestsCounted is the bulk build tests' key, whose drop, PartialEq::ne, and PartialOrd::lt the tests watch. */
typedef struct StdTestsCounted
{
	uint64		id;
} StdTestsCounted;

/* StdTestsCountedWatch is what the bulk build tests watch: the keys' drops in order, and the ne and lt calls to panic at. */
typedef struct StdTestsCountedWatch
{
	uint64		drops[64];
	size_t		drops_len;
	uint64		ne_panic;
	uint64		lt_panic;
} StdTestsCountedWatch;

/* std_btree_tests_WATCH is the bulk build tests' watch. */
static StdTestsCountedWatch std_btree_tests_WATCH;

/* std_btree_tests_Counted_drop is Counted's Drop: its id logged. */
static void
std_btree_tests_Counted_drop(StdTestsCounted *self)
{
	if (std_btree_tests_WATCH.drops_len < lengthof(std_btree_tests_WATCH.drops))
		std_btree_tests_WATCH.drops[std_btree_tests_WATCH.drops_len] = self->id;
	std_btree_tests_WATCH.drops_len++;
}

/* std_btree_tests_Counted_PartialEq_ne is Counted's PartialEq::ne: the ids', panicking at the chosen call. */
static bool
std_btree_tests_Counted_PartialEq_ne(const StdTestsCounted *self, const StdTestsCounted *other)
{
	if (std_btree_tests_WATCH.ne_panic != 0 && --std_btree_tests_WATCH.ne_panic == 0)
		std_panic(__FILE__, __LINE__, 0, "ne", sizeof("ne") - 1);
	return self->id != other->id;
}

/* std_btree_tests_Counted_PartialOrd_lt is Counted's PartialOrd::lt: the ids', panicking at the chosen call. */
static bool
std_btree_tests_Counted_PartialOrd_lt(const StdTestsCounted *self, const StdTestsCounted *other)
{
	if (std_btree_tests_WATCH.lt_panic != 0 && --std_btree_tests_WATCH.lt_panic == 0)
		std_panic(__FILE__, __LINE__, 0, "lt", sizeof("lt") - 1);
	return self->id < other->id;
}

/*
 * std_btree_tests_Counted_PartialOrd_lt_FnMut_call_mut is the fn item
 * Counted's PartialOrd::lt's FnMut shim, as the sort passes it for is_less.
 */
static bool
std_btree_tests_Counted_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_btree_tests_Counted_PartialOrd_lt(a, b);
}

/* std_btree_tests_Counted_Ord_cmp is Counted's Ord: the ids'. */
static StdOrdering
std_btree_tests_Counted_Ord_cmp(const void *a, const void *b)
{
	return std_u64_Ord_cmp(&((const StdTestsCounted *) a)->id, &((const StdTestsCounted *) b)->id);
}

/* The stable sort of Counted that the set's From<[T; N]> runs: core's shared sort parts and sort at Counted. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS Counted
#define STD_SORT_T StdTestsCounted
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS Counted
#define STD_STABLE_SORT_T StdTestsCounted
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT std_btree_tests_Counted_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

/* BTreeSet<Counted>, the bulk build tests' set, which they build from an array and drop. */
#define STD_BTREE_SCOPE extern
#define STD_BTREE_DECLARE
#define STD_BTREE_DEFINE
#define STD_BTREE_KS Counted
#define STD_BTREE_K StdTestsCounted
#define STD_BTREE_VS Set
#define STD_BTREE_V StdBTreeSetValZST
#define STD_BTREE_V_ZERO_SIZED
#define STD_BTREE_ITERATE
#define STD_BTREE_SET
#define STD_BTREE_DROP
#define STD_BTREE_BULK
#define STD_BTREE_KEY_NE(a, b) std_btree_tests_Counted_PartialEq_ne((a), (b))
#define STD_BTREE_SLICE_SORT(v, len) std_slice_sort_Counted((v), (len))
#define STD_BTREE_KEY_NEEDS_DROP
#define STD_BTREE_LAYOUT U64_SET
#define STD_BTREE_CMP(a, b) std_btree_tests_Counted_Ord_cmp((a), (b))
#define STD_BTREE_KEY_DROP(k) std_btree_tests_Counted_drop(k)
#define STD_BTREE_VAL_DROP(v) ((void) (v))
#define STD_BTREE_VAL_DEFAULT(v) ((void) (v))
#define STD_BTREE_TABLES
#define STD_BTREE_TABLE_ARGS
#include "doltgres/std/btree_template.h"

/* std_btree_tests_oom is the bulk build tests' allocation error hook: a failed allocation panics, as pg's hook unwinds. */
static void
std_btree_tests_oom(size_t size, size_t align)
{
	std_panic(__FILE__, __LINE__, 0, "out of memory", sizeof("out of memory") - 1);
}

/* std_btree_tests_OOM_HOOK is the bulk build tests' hook, set while a test fails an allocation. */
static const StdAllocErrorHook std_btree_tests_OOM_HOOK = {std_btree_tests_oom};

/* StdTestsCountedBuild is the bulk build tests' body's captures: the array, and the allocation to fail. */
typedef struct StdTestsCountedBuild
{
	StdTestsCounted *arr;
	uint64		fail_allocation;
} StdTestsCountedBuild;

/* std_btree_tests_build is the bulk build tests' body: the set built from the array, its allocation failing, then dropped. */
static void
std_btree_tests_build(void *captures)
{
	StdTestsCountedBuild *build = captures;
	StdBTreeMap set;

	test_std_alloc_tests_fail_allocation(build->fail_allocation);
	set = std_btree_BTreeSetCounted_FromArray_from(build->arr, 24);
	test_std_alloc_tests_fail_allocation(0);
	std_btree_BTreeMapCountedSet_drop(&set);
}

/*
 * std_btree_tests_run builds the set from the array {3, 1, 4, 1, 5, 9, 2,
 * 6, 5, 3, 5, 8, 9, 7, 9, 10, 11, 12, 13, 14, 2, 15, 16, 4}, its ne, its
 * lt, or its global allocator's nth allocation failing at the call chosen,
 * and checks whether it panicked and the keys it dropped, in order, against
 * Rust's.
 */
static void
std_btree_tests_run(uint64 ne_panic, uint64 lt_panic, uint64 fail_allocation, bool panics, const uint64 *drops,
					size_t drops_len)
{
	static const uint64 ids[24] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 10, 11, 12, 13, 14, 2, 15, 16, 4};
	StdTestsCounted arr[24];
	StdTestsCountedBuild build = {arr, fail_allocation};
	StdCaught	caught;
	bool		ok;

	for (int i = 0; i < 24; i++)
		arr[i].id = ids[i];
	memset(&std_btree_tests_WATCH, 0, sizeof(std_btree_tests_WATCH));
	std_btree_tests_WATCH.ne_panic = ne_panic;
	std_btree_tests_WATCH.lt_panic = lt_panic;
	std_alloc_set_alloc_error_hook(&std_btree_tests_OOM_HOOK);
	ok = std_catch_unwind(std_btree_tests_build, &build, &caught);
	test_std_alloc_tests_fail_allocation(0);
	std_alloc_set_alloc_error_hook(NULL);
	if (!ok)
		std_Caught_drop(&caught);
	if (ok == panics)
		std_panic(__FILE__, __LINE__, 0, "the build didn't panic as Rust's does",
				  sizeof("the build didn't panic as Rust's does") - 1);
	if (std_btree_tests_WATCH.drops_len != drops_len ||
		memcmp(std_btree_tests_WATCH.drops, drops, drops_len * sizeof(uint64)) != 0)
		std_panic(__FILE__, __LINE__, 0, "the keys dropped aren't Rust's", sizeof("the keys dropped aren't Rust's") - 1);
}

/*
 * test_std_btree_tests_a_set_from_an_array_drops_its_duplicates_then_its_keys
 * builds the set whole and drops it: the duplicates dropped as the build
 * meets them, each one's first kept, then the set's keys in order.
 */
void
test_std_btree_tests_a_set_from_an_array_drops_its_duplicates_then_its_keys(void)
{
	static const uint64 drops[] = {1, 2, 3, 4, 5, 5, 9, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

	std_btree_tests_run(0, 0, 0, false, drops, lengthof(drops));
}

/*
 * test_std_btree_tests_a_panicking_ne_drops_the_held_key_the_rest_then_the_peeked_one
 * panics the dedup's ne at its third call and at its first: the key it
 * holds dropped, then the array's rest, then the key peeked, the keys
 * already in the tree left, as Rust's are.
 */
void
test_std_btree_tests_a_panicking_ne_drops_the_held_key_the_rest_then_the_peeked_one(void)
{
	static const uint64 third[] = {1, 2, 3, 3, 4, 4, 5, 5, 5, 6, 7, 8, 9, 9, 9, 10, 11, 12, 13, 14, 15, 16, 2};
	static const uint64 first[] = {1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 6, 7, 8, 9, 9, 9, 10, 11, 12, 13, 14, 15, 16, 1};

	std_btree_tests_run(3, 0, 0, true, third, lengthof(third));
	std_btree_tests_run(1, 0, 0, true, first, lengthof(first));
}

/*
 * test_std_btree_tests_a_panicking_sort_drops_the_array_once panics the
 * sort's lt at its fifth call: every key of the array dropped once, in the
 * order the sort left them.
 */
void
test_std_btree_tests_a_panicking_sort_drops_the_array_once(void)
{
	static const uint64 drops[] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 10, 11, 12, 13, 14, 2, 15, 16, 4};

	std_btree_tests_run(0, 5, 0, true, drops, lengthof(drops));
}

/*
 * test_std_btree_tests_a_failed_allocation_drops_the_held_key_the_rest_then_the_peeked_one
 * fails the build's first allocation, its root's, and its third, the right
 * tree's: the array's keys dropped in sorted order, or the key held, the
 * array's rest, then the key peeked, as Rust's are.
 */
void
test_std_btree_tests_a_failed_allocation_drops_the_held_key_the_rest_then_the_peeked_one(void)
{
	static const uint64 root[] = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 6, 7, 8, 9, 9, 9, 10, 11, 12, 13, 14, 15, 16};
	static const uint64 right_tree[] = {1, 2, 3, 4, 5, 5, 9, 9, 12, 14, 15, 16, 13};

	std_btree_tests_run(0, 0, 1, true, root, lengthof(root));
	std_btree_tests_run(0, 0, 3, true, right_tree, lengthof(right_tree));
}

#endif							/* DOLTGRES_TEST_HOST */
