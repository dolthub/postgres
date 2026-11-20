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
 * Building trees bottom-up (trees' build.rs): the leaf entries a build takes
 * in key order, the items of a node at any level, writing a node of them,
 * and the Builder that gives the one tree those entries make.
 */
#ifndef DOLTGRES_TREES_BUILD_H
#define DOLTGRES_TREES_BUILD_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/store.h"

/*
 * TreesBuildEntry is Entry: a leaf entry's key, its value, the addresses its
 * value holds as (u32, Locator) offsets with their locators, its row stamps
 * in a tree that keeps them, and the value ids of its row's values out of
 * line.
 */
typedef struct TreesBuildEntry
{
	StdVec		key;
	StdVec		value;
	StdVec		addresses;
	StdVec		stamps;
	StdVec		values;
} TreesBuildEntry;

/*
 * TreesBuildEntriesOps is Entries' table: next_entry, true with *some false
 * after the last entry, true with *some and *result set for an entry, and
 * false with *error set for Some(Err).
 */
typedef struct TreesBuildEntriesOps
{
	bool		(*next_entry) (void *self, HostIoIo *io, bool *some, TreesBuildEntry *result, ChunksErrorError *error);
} TreesBuildEntriesOps;

/* TreesBuildEntries is entries in key order, a value that implements Entries. */
typedef struct TreesBuildEntries
{
	const TreesBuildEntriesOps *ops;
	void	   *self;
} TreesBuildEntries;

/* TreesBuildFromFn is FromFn: the function whose calls give the entries, with its captures and their drop. */
typedef struct TreesBuildFromFn
{
	bool		(*f) (void *captures, HostIoIo *io, bool *some, TreesBuildEntry *result, ChunksErrorError *error);
	void	   *f_captures;
	void		(*f_drop) (void *);
} TreesBuildFromFn;

/* TreesBuildItemTag is Item's variants. */
typedef enum TreesBuildItemTag
{
	TREES_BUILD_ITEM_LEAF,
	TREES_BUILD_ITEM_OLD,
	TREES_BUILD_ITEM_CHILD,
} TreesBuildItemTag;

/*
 * TreesBuildItem is Item: an entry of a node at any level, a leaf entry, an
 * entry of an old leaf read from its flat bytes, or an internal entry naming
 * a child with its last key, its subtree's entry count and highest tuple id,
 * and its subtree's summary in a tree that keeps them.
 */
typedef struct TreesBuildItem
{
	TreesBuildItemTag tag;
	union
	{
		TreesBuildEntry leaf;
		struct
		{
			StdArcSlice node;
			size_t		index;
			/* stamps is the old leaf's Arc<Stamps>. */
			StdArcInner *stamps;
		}			old;
		struct
		{
			StdVec		key;
			TreesStoreTree tree;
			uint64		count;
			uint64		max_tuple;
			bool		has_summary;
			StdVec		summary;
		}			child;
	}			u;
} TreesBuildItem;

/* TreesBuildLevel is Level: the node a level is building and how many nodes the level has finished. */
typedef struct TreesBuildLevel
{
	TreesChunkerSplitter splitter;
	/* items is the node's items, a Vec<Item>. */
	StdVec		items;
	uint64		finished;
} TreesBuildLevel;

/*
 * TreesBuildBuilder is Builder: a tree built from items in key order into
 * its store, at its target node size and in its shape, a level of nodes
 * being built for each level so far, from base, the level of the first
 * items.
 */
typedef struct TreesBuildBuilder
{
	TreesStoreNodeStore store;
	uint32		target;
	TreesShape	shape;
	/* levels is a Vec<Level>. */
	StdVec		levels;
	size_t		base;
} TreesBuildBuilder;

/* TREES_BUILD_FROM_FN_ENTRIES coerces a FromFn to the Entries it implements. */
#define TREES_BUILD_FROM_FN_ENTRIES(x) ((TreesBuildEntries) {&TREES_BUILD_FROM_FN_ENTRIES_OPS, (x)})

extern const TreesBuildEntriesOps TREES_BUILD_FROM_FN_ENTRIES_OPS;

extern TreesBuildFromFn trees_build_from_fn(bool (*f) (void *captures, HostIoIo *io, bool *some,
													   TreesBuildEntry *result, ChunksErrorError *error),
											void *f_captures, void (*f_drop) (void *));
extern bool trees_build_FromFn_Entries_next_entry(void *self, HostIoIo *io, bool *some, TreesBuildEntry *result,
												  ChunksErrorError *error);
extern void trees_build_FromFn_drop(TreesBuildFromFn *self);
extern void trees_build_Entry_Clone_clone(const TreesBuildEntry *self, TreesBuildEntry *result);
extern bool trees_build_Entry_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_build_Entry_PartialEq_eq(const TreesBuildEntry *self, const TreesBuildEntry *other);
extern void trees_build_Entry_drop(TreesBuildEntry *self);
extern StdBytes trees_build_Item_key(const TreesBuildItem *self);
extern uint64 trees_build_Item_size(const TreesBuildItem *self);
extern uint64 trees_build_Item_weight(const TreesBuildItem *self);
extern bool trees_build_Item_check(const TreesBuildItem *self, ChunksErrorError *error);
extern void trees_build_Item_Clone_clone(const TreesBuildItem *self, TreesBuildItem *result);
extern bool trees_build_Item_Debug_fmt(const void *value, StdFormatter *f);
extern void trees_build_Item_drop(TreesBuildItem *self);
extern void trees_build_VecItem_drop(StdVec *self);
extern bool trees_build_write_node(HostIoIo *io, TreesStoreNodeStore store, uint8 level, const TreesBuildItem *items,
								   size_t items_len, const TreesShape *shape, TreesBuildItem *result,
								   ChunksErrorError *error);

/*
 * TreesStoreBrandedBuilder is a Builder over a branded writer, which it
 * borrows until it finishes, and whose root its finish keeps in that writer
 * alone (store.c's).
 */
typedef struct TreesStoreBrandedBuilder
{
	TreesBuildBuilder builder;
	struct TreesStoreBrandedWriter *writer;
} TreesStoreBrandedBuilder;

extern void trees_store_BrandedWriter_shaped(struct TreesStoreBrandedWriter *self, uint32 target,
											 const TreesShape *shape, TreesStoreBrandedBuilder *result);
extern bool trees_store_BrandedBuilder_finish(TreesStoreBrandedBuilder *self, HostIoIo *io, size_t *result,
											  ChunksErrorError *error);
extern void trees_build_Builder_new(TreesStoreNodeStore store, uint32 target, TreesBuildBuilder *result);
extern void trees_build_Builder_shaped(TreesStoreNodeStore store, uint32 target, const TreesShape *shape,
									   TreesBuildBuilder *result);
extern void trees_build_Builder_at_level(TreesStoreNodeStore store, uint32 target, uint8 level,
										 const TreesShape *shape, TreesBuildBuilder *result);
extern bool trees_build_Builder_add(TreesBuildBuilder *self, HostIoIo *io, TreesBuildEntry *entry,
									ChunksErrorError *error);
extern bool trees_build_Builder_push(TreesBuildBuilder *self, HostIoIo *io, size_t level, TreesBuildItem *item,
									 ChunksErrorError *error);
extern bool trees_build_Builder_finish(TreesBuildBuilder *self, HostIoIo *io, const TreesStoreBrandedOps *branded,
									   TreesStoreFresh *result, ChunksErrorError *error);
extern bool trees_build_Builder_finish_tree(TreesBuildBuilder *self, HostIoIo *io, TreesStoreTree *result,
											ChunksErrorError *error);
extern void trees_build_Builder_drop(TreesBuildBuilder *self);

extern void trees_build_Entry_new_VecU8VecU8(StdVec *key, StdVec *value, TreesBuildEntry *result);

#ifdef DOLTGRES_TEST_TREES
extern void trees_build_Entry_new_VecU8Array8U8(StdVec *key, const uint8 *value, TreesBuildEntry *result);
#endif

#endif							/* DOLTGRES_TREES_BUILD_H */
