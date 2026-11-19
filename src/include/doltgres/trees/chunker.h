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
 * Where tree nodes end (trees' chunker.rs): a node ends after an entry when
 * the hash of its key falls below a threshold that rises with the node's
 * size, a Weibull distribution of node sizes computed with integers only.
 */
#ifndef DOLTGRES_TREES_CHUNKER_H
#define DOLTGRES_TREES_CHUNKER_H

#include "doltgres/std/vec.h"

/* TREES_CHUNKER_DEFAULT_TARGET is DEFAULT_TARGET, the default target node size. */
#define TREES_CHUNKER_DEFAULT_TARGET ((uint32) 8192)

/* TREES_CHUNKER_MAX_LEVEL is MAX_LEVEL, the highest level a tree can have: LEVEL_SEEDS' sixteen, less one. */
#define TREES_CHUNKER_MAX_LEVEL ((uint8) (16 - 1))

/*
 * TreesChunkerSplitter is Splitter: where the nodes of one level end, by
 * its level and target and the current node's size, whole size, count, and
 * at the leaves its last key.
 */
typedef struct TreesChunkerSplitter
{
	uint8		level;
	uint64		target;
	uint64		size;

	/*
	 * The node's size with every leaf key counted whole, which bounds the
	 * node once decoded.
	 */
	uint64		whole;
	uint32		count;
	/* At the leaves, the key of the node's last entry. */
	StdVec		last;
} TreesChunkerSplitter;

extern uint64 trees_chunker_mean_node_size(uint32 target);
extern uint64 trees_chunker_leaf_entry_size(const uint8 *key, size_t key_len, const uint8 *value, size_t value_len);
extern uint64 trees_chunker_internal_entry_size(const uint8 *key, size_t key_len);
extern TreesChunkerSplitter trees_chunker_Splitter_new(uint8 level, uint32 target);
extern bool trees_chunker_Splitter_add(TreesChunkerSplitter *self, const uint8 *key, size_t key_len, uint64 size);
extern bool trees_chunker_Splitter_is_empty(const TreesChunkerSplitter *self);
extern void trees_chunker_Splitter_drop(TreesChunkerSplitter *self);

#ifdef DOLTGRES_TEST_TREES
extern void test_trees_chunker_tests_node_sizes_follow_the_target(void);
extern void test_trees_chunker_tests_internal_nodes_never_end_after_one_entry(void);
#endif

#endif							/* DOLTGRES_TREES_CHUNKER_H */
