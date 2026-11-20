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
 * The B-tree instances' node layouts (the port text's section 4): rustc
 * 1.99.1's size, alignment, and field offsets of alloc's LeafNode<K, V> and
 * InternalNode<K, V> for each instance's K and V, which btree_template.h's
 * inclusions take.
 */
#ifndef DOLTGRES_STD_BTREE_LAYOUTS_H
#define DOLTGRES_STD_BTREE_LAYOUTS_H

/*
 * U64_ARC is BTreeMap<u64, Arc<T>>, a BackendLocal's map of actors, whose
 * value is a pointer whatever T is.
 */
#define STD_BTREE_LAYOUT_U64_ARC_LEAF_SIZE 192
#define STD_BTREE_LAYOUT_U64_ARC_LEAF_ALIGN 8
#define STD_BTREE_LAYOUT_U64_ARC_PARENT 0
#define STD_BTREE_LAYOUT_U64_ARC_PARENT_IDX 184
#define STD_BTREE_LAYOUT_U64_ARC_LEN 186
#define STD_BTREE_LAYOUT_U64_ARC_KEYS 8
#define STD_BTREE_LAYOUT_U64_ARC_VALS 96
#define STD_BTREE_LAYOUT_U64_ARC_INTERNAL_SIZE 288
#define STD_BTREE_LAYOUT_U64_ARC_INTERNAL_ALIGN 8
#define STD_BTREE_LAYOUT_U64_ARC_EDGES 192

/*
 * A64_U64 is BTreeMap<Aligned64, u64>, the test map whose keys are aligned
 * to 64 bytes, past the platform's malloc alignment.
 */
#define STD_BTREE_LAYOUT_A64_U64_LEAF_SIZE 832
#define STD_BTREE_LAYOUT_A64_U64_LEAF_ALIGN 64
#define STD_BTREE_LAYOUT_A64_U64_PARENT 704
#define STD_BTREE_LAYOUT_A64_U64_PARENT_IDX 800
#define STD_BTREE_LAYOUT_A64_U64_LEN 802
#define STD_BTREE_LAYOUT_A64_U64_KEYS 0
#define STD_BTREE_LAYOUT_A64_U64_VALS 712
#define STD_BTREE_LAYOUT_A64_U64_INTERNAL_SIZE 960
#define STD_BTREE_LAYOUT_A64_U64_INTERNAL_ALIGN 64
#define STD_BTREE_LAYOUT_A64_U64_EDGES 832

/*
 * U64_SET is BTreeSet<u64>, a BTreeMap<u64, SetValZST> whose zero-sized
 * values take no room in a node.
 */
#define STD_BTREE_LAYOUT_U64_SET_LEAF_SIZE 104
#define STD_BTREE_LAYOUT_U64_SET_LEAF_ALIGN 8
#define STD_BTREE_LAYOUT_U64_SET_PARENT 0
#define STD_BTREE_LAYOUT_U64_SET_PARENT_IDX 96
#define STD_BTREE_LAYOUT_U64_SET_LEN 98
#define STD_BTREE_LAYOUT_U64_SET_KEYS 8
#define STD_BTREE_LAYOUT_U64_SET_VALS 100
#define STD_BTREE_LAYOUT_U64_SET_INTERNAL_SIZE 200
#define STD_BTREE_LAYOUT_U64_SET_INTERNAL_ALIGN 8
#define STD_BTREE_LAYOUT_U64_SET_EDGES 104

/*
 * VEC_U8_ROW is BTreeMap<Vec<u8>, (Vec<u8>, Vec<Stamp>)>, the test writer's
 * rows by key, each with its tuple and its copies' stamps.
 */
#define STD_BTREE_LAYOUT_VEC_U8_ROW_LEAF_SIZE 808
#define STD_BTREE_LAYOUT_VEC_U8_ROW_LEAF_ALIGN 8
#define STD_BTREE_LAYOUT_VEC_U8_ROW_PARENT 528
#define STD_BTREE_LAYOUT_VEC_U8_ROW_PARENT_IDX 800
#define STD_BTREE_LAYOUT_VEC_U8_ROW_LEN 802
#define STD_BTREE_LAYOUT_VEC_U8_ROW_KEYS 536
#define STD_BTREE_LAYOUT_VEC_U8_ROW_VALS 0
#define STD_BTREE_LAYOUT_VEC_U8_ROW_INTERNAL_SIZE 904
#define STD_BTREE_LAYOUT_VEC_U8_ROW_INTERNAL_ALIGN 8
#define STD_BTREE_LAYOUT_VEC_U8_ROW_EDGES 808

#endif							/* DOLTGRES_STD_BTREE_LAYOUTS_H */
