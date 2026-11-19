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
 * Changing a tree by a batch of edits (trees' mutate.rs): the edits to leaf
 * entries, and the Merger that re-chunks each level from the start of the
 * node holding a run's first edit until a new node ends where an old one
 * did, which gives the same tree as building the changed entries from
 * scratch.
 */
#ifndef DOLTGRES_TREES_MUTATE_H
#define DOLTGRES_TREES_MUTATE_H

#include "doltgres/std/vec_deque.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/cursor.h"

/* TreesMutateEditTag is Edit's variants. */
typedef enum TreesMutateEditTag
{
	TREES_MUTATE_EDIT_PUT,
	TREES_MUTATE_EDIT_DELETE,
} TreesMutateEditTag;

/* TreesMutateEdit is Edit: a change to one leaf entry, a new entry for its key or its key's deletion. */
typedef struct TreesMutateEdit
{
	TreesMutateEditTag tag;
	union
	{
		TreesBuildEntry put;
		StdVec		delete;
	}			u;
} TreesMutateEdit;

/* trees_TupleVecU8OptionItem_t is the tuple (Vec<u8>, Option<Item>): a key edit to a level, its new item or none. */
typedef struct trees_TupleVecU8OptionItem_t
{
	StdVec		f0;
	bool		has_f1;
	TreesBuildItem f1;
} trees_TupleVecU8OptionItem_t;

/* trees_TupleVecU8Tree_t is the tuple (Vec<u8>, Tree): an old node a run passed, by its last key. */
typedef struct trees_TupleVecU8Tree_t
{
	StdVec		f0;
	TreesStoreTree f1;
} trees_TupleVecU8Tree_t;

/*
 * TreesMutateRun is Run: a run of re-chunking at a level, the stamps of the
 * old node at the cursor, the splitter of the new nodes, the items of the
 * node it is filling, the old nodes it passed by their last keys, the new
 * nodes it wrote, and how many leaf entries the nodes it writes must hold.
 */
typedef struct TreesMutateRun
{
	/* stamps is an Arc<Stamps>. */
	StdArcInner *stamps;
	TreesChunkerSplitter splitter;
	/* pending is a Vec<Item>. */
	StdVec		pending;
	/* consumed is a Vec<(Vec<u8>, Tree)>. */
	StdVec		consumed;
	/* produced is a Vec<Item>. */
	StdVec		produced;
	int64		expected;
} TreesMutateRun;

/*
 * TreesMutateLevel is Level: one level of a merger, a cursor over the tree's
 * nodes at the level, the edits to it not yet applied, the run under way,
 * and at the root's level every node the level now has.
 */
typedef struct TreesMutateLevel
{
	uint8		level;
	bool		has_cursor;
	TreesCursorCursor cursor;
	/* edits is a VecDeque<(Vec<u8>, Option<Item>)>. */
	StdVecDeque edits;
	bool		has_run;
	TreesMutateRun run;
	bool		has_made;
	/* made is a Vec<Item>. */
	StdVec		made;
} TreesMutateLevel;

/*
 * TreesMutateMerger is Merger: edits applied to a tree of a shape as they
 * come, writing nodes of the target size, its levels from the leaves up to
 * the root's, and whether an edit reached the root's level.
 */
typedef struct TreesMutateMerger
{
	TreesStoreTree tree;
	uint32		target;
	TreesShape	shape;
	/* levels is a Vec<Level>. */
	StdVec		levels;
	bool		reached;
} TreesMutateMerger;

extern void trees_mutate_Edit_Clone_clone(const void *self_, void *result_);
extern bool trees_mutate_Edit_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_mutate_Edit_PartialEq_eq(const TreesMutateEdit *self, const TreesMutateEdit *other);
extern void trees_mutate_Edit_drop(void *self_);
extern StdBytes trees_mutate_Edit_key(const TreesMutateEdit *self);
extern bool trees_mutate_mutate(HostIoIo *io, TreesStoreBranded store, TreesStoreNamed tree, uint32 target,
								StdVec *edits, const TreesShape *shape, TreesStoreFresh *result,
								ChunksErrorError *error);
extern bool trees_mutate_mutate_tree(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tree, uint32 target,
									 StdVec *edits, const TreesShape *shape, TreesStoreTree *result,
									 ChunksErrorError *error);
extern bool trees_mutate_Merger_new(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint32 target,
									const TreesShape *shape, TreesMutateMerger *result, ChunksErrorError *error);
extern bool trees_mutate_Merger_feed(TreesMutateMerger *self, HostIoIo *io, TreesStoreNodeStore store, StdVec *edits,
									 ChunksErrorError *error);
extern bool trees_store_BrandedWriter_finish_merger(TreesStoreBrandedWriter *self, TreesMutateMerger *merger,
													HostIoIo *io, size_t *result, ChunksErrorError *error);
extern bool trees_mutate_Merger_finish(TreesMutateMerger *self, HostIoIo *io, TreesStoreBranded store,
									   TreesStoreFresh *result, ChunksErrorError *error);
extern bool trees_mutate_Merger_finish_tree(TreesMutateMerger *self, HostIoIo *io, TreesStoreNodeStore store,
											TreesStoreTree *result, ChunksErrorError *error);
extern void trees_mutate_Merger_drop(TreesMutateMerger *self);
extern bool trees_mutate_check_run(HostIoIo *io, TreesStoreTree tree, uint8 level, const TreesBuildItem *produced,
								   size_t produced_len, int64 expected, const TreesKeyOrder *order,
								   ChunksErrorError *error);
extern bool trees_mutate_run_edits(HostIoIo *io, const trees_TupleVecU8Tree_t *consumed, size_t consumed_len,
								   const TreesBuildItem *produced, size_t produced_len, const TreesKeyOrder *order,
								   StdVec *above, ChunksErrorError *error);

/* pin's instance at Result<()>, which mutate.c defines. */
#define TREES_PIN_SCOPE extern
#define TREES_PIN_DECLARE
#define TREES_PIN_RS ResultUnit
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , ChunksErrorError *error
#include "doltgres/trees/pin_template.h"

#ifdef DOLTGRES_TEST_TREES
extern void test_trees_mutate_tests_a_merge_that_drops_an_old_entry_fails_instead_of_writing_the_tree(void);
#endif

#endif							/* DOLTGRES_TREES_MUTATE_H */
