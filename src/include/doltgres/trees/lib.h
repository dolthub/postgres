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
 * Prolly trees (trees' lib.rs): how a tree orders its keys and summarizes
 * its subtrees, by bytes or by an engine's functions under the locks a
 * thread holds or only with its Io, and the shape a tree is kept in.
 */
#ifndef DOLTGRES_TREES_LIB_H
#define DOLTGRES_TREES_LIB_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/std/any.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/store.h"

/* TreesSummaryFn is SummaryFn: a summary kind's summary of leaf values at level 0 or of children's summaries above. */
typedef bool (*TreesSummaryFn) (uint8 level, const StdBytes *items, size_t items_len, StdVec *result,
								ChunksErrorError *error);

/*
 * TreesTreeSummarizerOps is TreeSummarizer's table: its Any identity, which
 * gives the value's drop, size, and alignment, then summarize under the held
 * locks and same_definition.
 */
typedef struct TreesTreeSummarizerOps
{
	const StdTypeId *type;
	bool		(*summarize) (void *self, HostIoHeld *held, uint8 level, const StdBytes *items, size_t items_len,
							  StdVec *result, ChunksErrorError *error);
	bool		(*same_definition) (void *self, const struct TreesTreeSummarizerOps *other_ops, void *other);
} TreesTreeSummarizerOps;

/* TreesIoSummarizerOps is IoSummarizer's table: its Any identity, then summarize with the Io and same_definition. */
typedef struct TreesIoSummarizerOps
{
	const StdTypeId *type;
	bool		(*summarize) (void *self, HostIoIo *io, uint8 level, const StdBytes *items, size_t items_len,
							  StdVec *result, ChunksErrorError *error);
	bool		(*same_definition) (void *self, const struct TreesIoSummarizerOps *other_ops, void *other);
} TreesIoSummarizerOps;

/* TreesKeyComparatorOps is KeyComparator's table: its Any identity, then compare under the held locks. */
typedef struct TreesKeyComparatorOps
{
	const StdTypeId *type;
	bool		(*compare) (void *self, HostIoHeld *held, const uint8 *a, size_t a_len, const uint8 *b, size_t b_len,
							StdOrdering *result, ChunksErrorError *error);
} TreesKeyComparatorOps;

/* TreesIoComparatorOps is IoComparator's table: its Any identity, then compare with the Io. */
typedef struct TreesIoComparatorOps
{
	const StdTypeId *type;
	bool		(*compare) (void *self, HostIoIo *io, const uint8 *a, size_t a_len, const uint8 *b, size_t b_len,
							StdOrdering *result, ChunksErrorError *error);
} TreesIoComparatorOps;

/* TreesArcTreeSummarizer is an Arc<dyn TreeSummarizer>: its block and its table. */
typedef struct TreesArcTreeSummarizer
{
	StdArcInner *inner;
	const TreesTreeSummarizerOps *ops;
} TreesArcTreeSummarizer;

/* TreesArcIoSummarizer is an Arc<dyn IoSummarizer>: its block and its table. */
typedef struct TreesArcIoSummarizer
{
	StdArcInner *inner;
	const TreesIoSummarizerOps *ops;
} TreesArcIoSummarizer;

/* TreesArcKeyComparator is an Arc<dyn KeyComparator>: its block and its table. */
typedef struct TreesArcKeyComparator
{
	StdArcInner *inner;
	const TreesKeyComparatorOps *ops;
} TreesArcKeyComparator;

/* TreesArcIoComparator is an Arc<dyn IoComparator>: its block and its table. */
typedef struct TreesArcIoComparator
{
	StdArcInner *inner;
	const TreesIoComparatorOps *ops;
} TreesArcIoComparator;

/* TreesSummarizerTag is Summarizer's variants. */
typedef enum TreesSummarizerTag
{
	TREES_SUMMARIZER_FN,
	TREES_SUMMARIZER_DYN,
	TREES_SUMMARIZER_DYN_IO,
} TreesSummarizerTag;

/* TreesSummarizer is Summarizer: a summary kind's function, or an engine's that runs under held locks or only with the Io. */
typedef struct TreesSummarizer
{
	TreesSummarizerTag tag;
	union
	{
		TreesSummaryFn fn;
		TreesArcTreeSummarizer dyn;
		TreesArcIoSummarizer dyn_io;
	}			u;
} TreesSummarizer;

/* TreesHeldSummarizerTag is HeldSummarizer's variants. */
typedef enum TreesHeldSummarizerTag
{
	TREES_HELD_SUMMARIZER_FN,
	TREES_HELD_SUMMARIZER_DYN,
} TreesHeldSummarizerTag;

/* TreesHeldSummarizer is HeldSummarizer: a summarizer that runs under the held locks, borrowed. */
typedef struct TreesHeldSummarizer
{
	TreesHeldSummarizerTag tag;
	union
	{
		TreesSummaryFn fn;
		struct
		{
			const TreesTreeSummarizerOps *ops;
			void	   *self;
		}			dyn;
	}			u;
} TreesHeldSummarizer;

/* TreesKeyOrderTag is KeyOrder's variants, Bytes its Default. */
typedef enum TreesKeyOrderTag
{
	TREES_KEY_ORDER_BYTES,
	TREES_KEY_ORDER_COMPARE,
	TREES_KEY_ORDER_COMPARE_IO,
} TreesKeyOrderTag;

/* TreesKeyOrder is KeyOrder: a tree's keys in byte order, or by a comparator that runs under held locks or only with the Io. */
typedef struct TreesKeyOrder
{
	TreesKeyOrderTag tag;
	union
	{
		TreesArcKeyComparator compare;
		TreesArcIoComparator compare_io;
	}			u;
} TreesKeyOrder;

/* TreesHeldKeyOrder is HeldKeyOrder: byte order, NULL ops, or a borrowed comparator that runs under held locks. */
typedef struct TreesHeldKeyOrder
{
	struct
	{
		const TreesKeyComparatorOps *ops;
		void	   *self;
	}			f0;
} TreesHeldKeyOrder;

/* TreesShape is Shape: a tree's summarizer of each child's subtree, if any, and the order of its keys. */
typedef struct TreesShape
{
	bool		has_summarizer;
	TreesSummarizer summarizer;
	TreesKeyOrder order;
} TreesShape;

extern bool trees_Summarizer_same_definition(const TreesSummarizer *self, const TreesSummarizer *other);
extern bool trees_Summarizer_summarize(const TreesSummarizer *self, HostIoIo *io, uint8 level, const StdBytes *items,
									   size_t items_len, StdVec *result, ChunksErrorError *error);
extern bool trees_Summarizer_functions(const TreesSummarizer *self, StdAny *result);
extern bool trees_Summarizer_held(const TreesSummarizer *self, TreesHeldSummarizer *result);
extern bool trees_Summarizer_Debug_fmt(const void *value, StdFormatter *f);
extern void trees_Summarizer_Clone_clone(const TreesSummarizer *self, TreesSummarizer *result);
extern void trees_Summarizer_drop(TreesSummarizer *self);
extern bool trees_HeldSummarizer_summarize(const TreesHeldSummarizer *self, HostIoHeld *held, uint8 level,
										   const StdBytes *items, size_t items_len, StdVec *result,
										   ChunksErrorError *error);
extern bool trees_KeyOrder_compare(const TreesKeyOrder *self, HostIoIo *io, const uint8 *a, size_t a_len,
								   const uint8 *b, size_t b_len, StdOrdering *result, ChunksErrorError *error);
extern bool trees_KeyOrder_held(const TreesKeyOrder *self, TreesHeldKeyOrder *result);

/* trees_TupleVecU8Bool_t is (Vec<u8>, bool): a sampled leaf's last key and whether its leaf follows the one before. */
typedef struct trees_TupleVecU8Bool_t
{
	StdVec		f0;
	bool		f1;
} trees_TupleVecU8Bool_t;

/*
 * TreesSpreadKeys is SpreadKeys: keys of a tree spread evenly over it, in key
 * order, the last key of each leaf under some nodes of the level above the
 * leaves, and every key of some of those leaves.
 */
typedef struct TreesSpreadKeys
{
	/*
	 * lasts is each sampled leaf's last key and whether its leaf follows the
	 * leaf of the key before it.
	 */
	StdVec		lasts;
	/* lasts_entries is the entries the sampled leaves hold. */
	uint64		lasts_entries;
	/* leaves is every key of each fully read leaf, a Vec<Vec<Vec<u8>>>. */
	StdVec		leaves;

	/*
	 * leaf_lasts is where in lasts each fully read leaf's last key is, empty
	 * when the tree is one leaf.
	 */
	StdVec		leaf_lasts;
	/* entries is the entries the tree holds. */
	uint64		entries;
} TreesSpreadKeys;

extern bool trees_KeyOrder_functions(const TreesKeyOrder *self, StdAny *result);
extern bool trees_KeyOrder_is_bytes(const TreesKeyOrder *self);
extern bool trees_KeyOrder_Debug_fmt(const void *value, StdFormatter *f);
extern void trees_KeyOrder_Clone_clone(const TreesKeyOrder *self, TreesKeyOrder *result);
extern TreesKeyOrder trees_KeyOrder_Default_default(void);
extern void trees_KeyOrder_drop(TreesKeyOrder *self);
extern bool trees_HeldKeyOrder_compare(const TreesHeldKeyOrder *self, HostIoHeld *held, const uint8 *a, size_t a_len,
									   const uint8 *b, size_t b_len, StdOrdering *result, ChunksErrorError *error);
extern bool trees_sorted_positions(HostIoIo *io, const StdBytes *keys, size_t keys_len, const TreesKeyOrder *order,
								   StdVec *result, ChunksErrorError *error);
extern void trees_Shape_Clone_clone(const TreesShape *self, TreesShape *result);
extern bool trees_Shape_Debug_fmt(const void *value, StdFormatter *f);
extern TreesShape trees_Shape_Default_default(void);
extern void trees_Shape_drop(TreesShape *self);
extern bool trees_get(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, const uint8 *key, size_t key_len,
					  bool *some, StdVec *result, ChunksErrorError *error);
extern bool trees_count(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint64 *result,
						ChunksErrorError *error);
extern bool trees_max_tuple(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint64 *result,
							ChunksErrorError *error);
extern bool trees_subtree_max(const ChunksTrailerStamps *stamps, size_t i, uint64 *result, ChunksErrorError *error);
extern bool trees_leaves(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint64 *result,
						 ChunksErrorError *error);
extern bool trees_node_counts(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, std_TupleU64U64_t *result,
							  ChunksErrorError *error);
extern TreesSpreadKeys trees_SpreadKeys_Default_default(void);
extern void trees_VecTupleVecU8Bool_drop(StdVec *self);
extern void trees_VecVecU8_drop(StdVec *self);
extern void trees_VecVecVecU8_drop(StdVec *self);
extern void trees_SpreadKeys_drop(TreesSpreadKeys *self);
extern bool trees_spread_keys(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, size_t nodes,
							  size_t leaves, TreesSpreadKeys *result, ChunksErrorError *error);
extern bool trees_height(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint8 *result,
						 ChunksErrorError *error);
extern bool trees_empty(HostIoIo *io, TreesStoreBranded store, TreesStoreFresh *result, ChunksErrorError *error);

#endif							/* DOLTGRES_TREES_LIB_H */
