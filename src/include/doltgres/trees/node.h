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
 * Tree nodes (trees' node.rs): a node's hashed bytes, decoded into flat
 * bytes that a Node reads in place, and the encoders of leaves and internal
 * nodes.
 */
#ifndef DOLTGRES_TREES_NODE_H
#define DOLTGRES_TREES_NODE_H

#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/cache.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/store.h"

/*
 * TreesNodeNode is Node: a decoded tree node, viewed in its flat bytes: a
 * header, the node's hashed bytes with a leaf's keys written out whole and
 * keys that vary in length after a four-byte offset per key, an internal
 * node's subtree counts, a leaf's address table, and the locators and stamps
 * of the stored chunk's trailer as it stores them.
 */
typedef struct TreesNodeNode
{
	ChunksAddressAddress address;
	StdBytes	bytes;
	uint8		level;
	size_t		count;
	/* body is the offset of the body, after the chunk header. */
	size_t		body;
	size_t		key_width;
	size_t		keys_at;
	size_t		key_data_at;
	size_t		values_at;
	size_t		value_data_at;

	/*
	 * prefix is, for an internal node, the entry count of the subtrees before
	 * each child and after the last, eight bytes each.
	 */
	StdBytes	prefix;

	/*
	 * summaries_at is, for an internal node with summaries, where their
	 * offsets start, or 0.
	 */
	size_t		summaries_at;

	/*
	 * table is, for a leaf, the body offsets of the addresses its values
	 * hold, in increasing order, four bytes each.
	 */
	StdBytes	table;
	/* blob is whether the node is a blob leaf, whose body is raw bytes. */
	bool		blob;

	/*
	 * comparator is whether the node's tree orders its keys by a comparator
	 * rather than by their bytes.
	 */
	bool		comparator;

	/*
	 * locators are the trailer's locators, of the node's address table in
	 * order.
	 */
	StdBytes	locators;
	/* stamps are the trailer's stamps, decoded, eight-byte aligned. */
	StdBytes	stamps;
} TreesNodeNode;

/* TreesNodeFlat is Flat: a node's flat bytes as a store read them, pinned in a node cache or the read's own. */
typedef struct TreesNodeFlat
{
	HostCacheRead f0;
} TreesNodeFlat;

extern size_t trees_node_shared_prefix(const uint8 *a, size_t a_len, const uint8 *b, size_t b_len);
extern bool trees_node_flatten(const ChunksChunkChunk *chunk, StdVec *result, ChunksErrorError *error);
extern bool trees_node_Flat_of(const ChunksChunkChunk *chunk, TreesNodeFlat *result, ChunksErrorError *error);
extern TreesNodeNode trees_node_Flat_node(const TreesNodeFlat *self);
extern void trees_node_Flat_drop(TreesNodeFlat *self);
extern bool trees_node_Node_Debug_fmt(const void *value, StdFormatter *f);
extern TreesNodeNode trees_node_Node_view(const uint8 *flat, size_t flat_len);
extern ChunksTrailerStamps trees_node_Node_stamps_view(const TreesNodeNode *self);
extern const uint32 *trees_node_Node_values(const TreesNodeNode *self, size_t i, size_t *result_len);
extern void trees_node_Node_stamps(const TreesNodeNode *self, ChunksTrailerStamps *result);
extern ChunksAddressAddress trees_node_Node_address(const TreesNodeNode *self);
extern StdBytes trees_node_Node_bytes(const TreesNodeNode *self);
extern uint8 trees_node_Node_level(const TreesNodeNode *self);
extern bool trees_node_Node_is_leaf(const TreesNodeNode *self);
extern size_t trees_node_Node_len(const TreesNodeNode *self);
extern bool trees_node_Node_is_empty(const TreesNodeNode *self);
extern StdBytes trees_node_Node_key(const TreesNodeNode *self, size_t i);
extern StdBytes trees_node_Node_value(const TreesNodeNode *self, size_t i);
extern ChunksAddressAddress trees_node_Node_child(const TreesNodeNode *self, size_t i);
extern ChunksTrailerLocator trees_node_Node_locator(const TreesNodeNode *self, size_t i);
extern TreesStoreTree trees_node_Node_child_tree(const TreesNodeNode *self, size_t i);
extern void trees_node_Node_value_addresses(const TreesNodeNode *self, size_t i, StdVec *result);
extern void trees_node_Node_value_addresses_into(const TreesNodeNode *self, size_t i, StdVec *addresses);
extern StdBytes trees_node_Node_summary(const TreesNodeNode *self, size_t i);
extern uint64 trees_node_Node_subtree(const TreesNodeNode *self, size_t i);
extern uint64 trees_node_Node_before(const TreesNodeNode *self, size_t i);
extern uint64 trees_node_Node_total(const TreesNodeNode *self);
extern StdBytes trees_node_Node_blob_data(const TreesNodeNode *self);
extern bool trees_node_Node_search(const TreesNodeNode *self, HostIoIo *io, const uint8 *key, size_t key_len,
								   const TreesKeyOrder *order, size_t *result, ChunksErrorError *error);
extern bool trees_node_Node_search_from(const TreesNodeNode *self, HostIoIo *io, const uint8 *key, size_t key_len,
										const TreesKeyOrder *order, size_t from, size_t *result,
										ChunksErrorError *error);
extern bool trees_node_Node_search_past(const TreesNodeNode *self, HostIoIo *io, const uint8 *key, size_t key_len,
										const TreesKeyOrder *order, size_t *result, ChunksErrorError *error);
extern void trees_node_encode_leaf(const StdBytes *keys, size_t keys_len, const StdBytes *values, size_t values_len,
								   const StdVec *addresses, size_t addresses_len, bool comparator, StdVec *result);
extern void trees_node_encode_internal(uint8 level, const StdBytes *keys, size_t keys_len,
									   const ChunksAddressAddress *children, size_t children_len, const uint64 *counts,
									   size_t counts_len, StdVec *result);
extern void trees_node_encode_internal_with_summaries(uint8 level, const StdBytes *keys, size_t keys_len,
													  const ChunksAddressAddress *children, size_t children_len,
													  const uint64 *counts, size_t counts_len,
													  const StdBytes *summaries, size_t summaries_len,
													  bool comparator, StdVec *result);

extern bool trees_node_TupleU32Locator_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_node_SliceTupleU32Locator_Debug_fmt(const chunks_TupleU32Locator_t *self, size_t self_len,
													  StdFormatter *f);
extern bool trees_node_VecTupleU32Locator_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_node_SliceTupleU32Locator_PartialEqSliceTupleU32Locator_eq(const chunks_TupleU32Locator_t *self,
																			 size_t self_len,
																			 const chunks_TupleU32Locator_t *other,
																			 size_t other_len);
extern bool trees_node_VecTupleU32Locator_PartialEqVecTupleU32Locator_eq(const StdVec *self, const StdVec *other);
#ifdef DOLTGRES_TEST_TREES
extern void test_trees_node_tests_leaves_and_internal_nodes_round_trip(void);
extern void test_trees_node_tests_front_coded_leaf_keys_round_trip_at_prefix_boundaries(void);
extern void test_trees_node_tests_refuses_offsets_and_counts_its_accessors_cannot_read(void);
extern void test_trees_node_tests_a_flat_nodes_stamps_read_in_place_as_its_trailer_decodes(void);
#endif

#endif							/* DOLTGRES_TREES_NODE_H */
