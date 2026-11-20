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
 * The C form of Rust's BTreeMap (the port text's sections 2 and 9): the
 * parts of alloc/src/collections/btree at Rust 1.99.1 that hold no key or
 * value, which every instance of btree_template.h shares: node.rs's
 * constants, its node references and handles, the map and its occupied
 * entries, search.rs's results, and node.rs's splitpoint and move_to_slice.
 */
#ifndef DOLTGRES_STD_BTREE_H
#define DOLTGRES_STD_BTREE_H

#include "doltgres/std/cmp.h"
#include "doltgres/std/vec.h"

/* STD_BTREE_B is node.rs's B: a node's edges are twice it. */
#define STD_BTREE_B 6

/* STD_BTREE_CAPACITY is node.rs's CAPACITY, a node's room for keys and values. */
#define STD_BTREE_CAPACITY (2 * STD_BTREE_B - 1)

/* STD_BTREE_KV_IDX_CENTER is the key and value a full node splits around. */
#define STD_BTREE_KV_IDX_CENTER (STD_BTREE_B - 1)

/* STD_BTREE_EDGE_IDX_LEFT_OF_CENTER is the edge left of the center. */
#define STD_BTREE_EDGE_IDX_LEFT_OF_CENTER (STD_BTREE_B - 1)

/* STD_BTREE_EDGE_IDX_RIGHT_OF_CENTER is the edge right of the center. */
#define STD_BTREE_EDGE_IDX_RIGHT_OF_CENTER STD_BTREE_B

/* STD_BTREE_DIR is the B-tree's directory in std's source, which its panics name. */
#define STD_BTREE_DIR STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "collections" STD_SEP "btree" STD_SEP

/* STD_BTREE_NAVIGATE_FILE is navigate.rs. */
#define STD_BTREE_NAVIGATE_FILE STD_BTREE_DIR "navigate.rs"

/* STD_BTREE_NODE_FILE is node.rs. */
#define STD_BTREE_NODE_FILE STD_BTREE_DIR "node.rs"

/* STD_BTREE_ARRAY_ITER_INNER_FILE is core's array/iter/iter_inner.rs, where an array IntoIter's drop takes its alive keys. */
#define STD_BTREE_ARRAY_ITER_INNER_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "array" STD_SEP "iter" STD_SEP "iter_inner.rs"

/* STD_BTREE_ENTRY_FILE is map/entry.rs. */
#define STD_BTREE_ENTRY_FILE STD_BTREE_DIR "map" STD_SEP "entry.rs"

/* STD_BTREE_BOXED_FILE is alloc's boxed.rs, where Box::into_non_null_with_allocator's check reports. */
#define STD_BTREE_BOXED_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "boxed.rs"

/* STD_BTREE_OPTION_FILE is core's option.rs, where Option::insert's unwrap_unchecked reports. */
#define STD_BTREE_OPTION_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "option.rs"

/* StdBTreeLeafNode is an instance's LeafNode<K, V>, laid out at the instance's measured offsets. */
typedef struct StdBTreeLeafNode StdBTreeLeafNode;

/* StdBTreeInternalNode is an instance's InternalNode<K, V>, whose leaf part comes first. */
typedef struct StdBTreeInternalNode StdBTreeInternalNode;

/*
 * StdBTreeNodeRef is NodeRef<BorrowType, K, V, Type>: a node and its
 * height, one C form for every borrow type and node type.
 */
typedef struct StdBTreeNodeRef
{
	size_t		height;
	StdBTreeLeafNode *node;
} StdBTreeNodeRef;

/* StdBTreeHandle is Handle<Node, Type>: a node and the index of a key and value, or of an edge, in it. */
typedef struct StdBTreeHandle
{
	StdBTreeNodeRef node;
	size_t		idx;
} StdBTreeHandle;

/* StdBTreeMap is BTreeMap<K, V>: its root, none while the root's node is NULL, and its length. */
typedef struct StdBTreeMap
{
	StdBTreeNodeRef root;
	size_t		length;
} StdBTreeMap;

/* StdBTreeLazyLeafHandleTag is LazyLeafHandle's variants. */
typedef enum StdBTreeLazyLeafHandleTag
{
	STD_BTREE_LAZY_LEAF_HANDLE_ROOT,
	STD_BTREE_LAZY_LEAF_HANDLE_EDGE,
} StdBTreeLazyLeafHandleTag;

/* StdBTreeLazyLeafHandle is navigate.rs's LazyLeafHandle: a root not yet descended, or a leaf's edge. */
typedef struct StdBTreeLazyLeafHandle
{
	StdBTreeLazyLeafHandleTag tag;
	union
	{
		StdBTreeNodeRef root;
		StdBTreeHandle edge;
	}			u;
} StdBTreeLazyLeafHandle;

/* StdBTreeLazyLeafRange is navigate.rs's LazyLeafRange: its front and its back, each when it has one. */
typedef struct StdBTreeLazyLeafRange
{
	bool		has_front;
	StdBTreeLazyLeafHandle front;
	bool		has_back;
	StdBTreeLazyLeafHandle back;
} StdBTreeLazyLeafRange;

/* StdBTreeIter is map.rs's Iter: its range and the entries left in it. */
typedef struct StdBTreeIter
{
	StdBTreeLazyLeafRange range;
	size_t		length;
} StdBTreeIter;

/* STD_BTREE_MIN_LEN is map.rs's MIN_LEN, node.rs's MIN_LEN_AFTER_SPLIT: B - 1, the fewest a non-root node holds. */
#define STD_BTREE_MIN_LEN (STD_BTREE_B - 1)

/* StdBTreeBalancingContext is node.rs's BalancingContext: a parent's key and value, and its children either side. */
typedef struct StdBTreeBalancingContext
{
	StdBTreeHandle parent;
	StdBTreeNodeRef left_child;
	StdBTreeNodeRef right_child;
} StdBTreeBalancingContext;

/* StdBTreeIntoIter is map.rs's IntoIter: its range over the nodes it takes and the entries left in it. */
typedef struct StdBTreeIntoIter
{
	StdBTreeLazyLeafRange range;
	size_t		length;
} StdBTreeIntoIter;

/* StdBTreeKeys is map.rs's Keys: its inner Iter. */
typedef struct StdBTreeKeys
{
	StdBTreeIter inner;
} StdBTreeKeys;

/* StdBTreeSetIter is set.rs's Iter: the Keys of its map. */
typedef struct StdBTreeSetIter
{
	StdBTreeKeys iter;
} StdBTreeSetIter;

/* StdBTreeSetValZST is alloc's SetValZST, a BTreeSet's map's zero-sized value, which a node never holds. */
typedef struct StdBTreeSetValZST
{
	char		unused;
} StdBTreeSetValZST;

/* STD_BTREE_MAP_NEW is BTreeMap::new, which allocates nothing, for a static's initializer. */
#define STD_BTREE_MAP_NEW {{0, NULL}, 0}

/* StdBTreeOccupiedEntry is OccupiedEntry<K, V>: its key and value's handle and its map. */
typedef struct StdBTreeOccupiedEntry
{
	StdBTreeHandle handle;
	StdBTreeMap *dormant_map;
} StdBTreeOccupiedEntry;

/* StdBTreeEntryTag is Entry's variants. */
typedef enum StdBTreeEntryTag
{
	STD_BTREE_ENTRY_VACANT,
	STD_BTREE_ENTRY_OCCUPIED,
} StdBTreeEntryTag;

/* StdBTreeSearchResultTag is SearchResult's variants. */
typedef enum StdBTreeSearchResultTag
{
	STD_BTREE_SEARCH_RESULT_FOUND,
	STD_BTREE_SEARCH_RESULT_GO_DOWN,
} StdBTreeSearchResultTag;

/* StdBTreeSearchResult is SearchResult: the found key's handle, or the edge to go down. */
typedef struct StdBTreeSearchResult
{
	StdBTreeSearchResultTag tag;
	union
	{
		StdBTreeHandle found;
		StdBTreeHandle go_down;
	}			u;
} StdBTreeSearchResult;

/* StdBTreeIndexResultTag is IndexResult's variants. */
typedef enum StdBTreeIndexResultTag
{
	STD_BTREE_INDEX_RESULT_KV,
	STD_BTREE_INDEX_RESULT_EDGE,
} StdBTreeIndexResultTag;

/* StdBTreeIndexResult is IndexResult: the index of the key found in a node, or of the edge it belongs below. */
typedef struct StdBTreeIndexResult
{
	StdBTreeIndexResultTag tag;
	union
	{
		size_t		kv;
		size_t		edge;
	}			u;
} StdBTreeIndexResult;

/* StdBTreeLeftOrRightTag is LeftOrRight's variants. */
typedef enum StdBTreeLeftOrRightTag
{
	STD_BTREE_LEFT_OR_RIGHT_LEFT,
	STD_BTREE_LEFT_OR_RIGHT_RIGHT,
} StdBTreeLeftOrRightTag;

/* StdBTreeLeftOrRight is LeftOrRight<usize>: an index in the left or the right half of a split. */
typedef struct StdBTreeLeftOrRight
{
	StdBTreeLeftOrRightTag tag;
	union
	{
		size_t		left;
		size_t		right;
	}			u;
} StdBTreeLeftOrRight;

extern void std_btree_splitpoint(size_t edge_idx, size_t *out_0, StdBTreeLeftOrRight *out_1);
extern void std_btree_move_to_slice(void *src, size_t src_len, void *dst, size_t dst_len, size_t size,
									size_t align);

#ifdef DOLTGRES_TEST_HOST
extern void test_std_btree_tests_an_over_aligned_key_stays_aligned(void);
extern void test_std_btree_tests_a_set_from_an_array_drops_its_duplicates_then_its_keys(void);
extern void test_std_btree_tests_a_panicking_ne_drops_the_held_key_the_rest_then_the_peeked_one(void);
extern void test_std_btree_tests_a_panicking_sort_drops_the_array_once(void);
extern void test_std_btree_tests_a_failed_allocation_drops_the_held_key_the_rest_then_the_peeked_one(void);
#endif

#endif							/* DOLTGRES_STD_BTREE_H */
