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
 * Cursors (trees' cursor.rs): a position at one level of a tree, its path of
 * frames from the root down and its own node, the form a cursor is kept in
 * across calls, and the row stamps a walk keeps of the leaf it last read.
 */
#ifndef DOLTGRES_TREES_CURSOR_H
#define DOLTGRES_TREES_CURSOR_H

#include "doltgres/host/cache.h"
#include "doltgres/std/sync.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/pin.h"
#include "doltgres/trees/store.h"

/* The OnceCell<Arc<[u8]>> instance, a cursor's copy of its node's flat bytes. */
#define STD_ONCE_CELL_SCOPE extern
#define STD_ONCE_CELL_DECLARE
#define STD_ONCE_CELL_TS ArcSliceU8
#define STD_ONCE_CELL_T StdArcSlice
#define STD_ONCE_CELL_DROP trees_cursor_ArcSliceU8_drop
#include "doltgres/std/once_cell_template.h"

/*
 * TREES_CURSOR_DEFAULT_READ_AHEAD is DEFAULT_READ_AHEAD, how many leaves past
 * the one a forward move enters are requested ahead of being read unless a
 * cursor is told otherwise, Postgres' default effective_io_concurrency.
 */
#define TREES_CURSOR_DEFAULT_READ_AHEAD 16

/* TreesCursorFrame is Frame: one node on a cursor's path, where it was stored, the position in it, its entry count, and its level. */
typedef struct TreesCursorFrame
{
	TreesStoreTree tree;
	size_t		idx;
	size_t		len;
	uint8		level;
} TreesCursorFrame;

/*
 * TreesCursorCursor is Cursor: a position at one level of a tree, valid
 * while it is at an entry, which reads its own node within the entry that
 * made or resumed it.
 */
typedef struct TreesCursorCursor
{
	/*
	 * path is the path from the root down, the cursor's own node last, a
	 * Vec<Frame>.
	 */
	StdVec		path;

	/*
	 * node is the cursor's own node, once a seek has reached it: a read of
	 * its own, which outlives the scopes it moves in.
	 */
	bool		has_node;
	TreesNodeFlat node;

	/*
	 * copy is a copy of the node's flat bytes, made the first time something
	 * that outlives the cursor's stay in the node asks for one.
	 */
	StdOnceCellArcSliceU8 copy;
	bool		valid;

	/*
	 * read_ahead is how many leaves past the one a forward move enters are
	 * requested ahead of being read.
	 */
	size_t		read_ahead;
} TreesCursorCursor;

/* TreesCursorStayTag is Stay's variants. */
typedef enum TreesCursorStayTag
{
	TREES_CURSOR_STAY_PINNED,
	TREES_CURSOR_STAY_OWN,
} TreesCursorStayTag;

/* TreesCursorStay is Stay: a cursor's own node as it is kept between calls, a kept pin or a read's own flat bytes. */
typedef struct TreesCursorStay
{
	TreesCursorStayTag tag;
	union
	{
		HostCacheKept pinned;
		StdVec		own;
	}			u;
} TreesCursorStay;

/* TreesCursorKeptCursor is KeptCursor: a cursor kept across calls, its path, and its own node as a Stay. */
typedef struct TreesCursorKeptCursor
{
	StdVec		path;
	bool		has_node;
	TreesCursorStay node;
	bool		valid;
	size_t		read_ahead;
} TreesCursorKeptCursor;

/* trees_TupleTreeArcStamps_t is the tuple (Tree, Arc<Stamps>): a leaf and its row stamps. */
typedef struct trees_TupleTreeArcStamps_t
{
	TreesStoreTree f0;
	StdArcInner *f1;
} trees_TupleTreeArcStamps_t;

/* TreesCursorLeafStamps is LeafStamps: the row stamps of the leaf a walk last read them in, if any. */
typedef struct TreesCursorLeafStamps
{
	bool		has_f0;
	trees_TupleTreeArcStamps_t f0;
} TreesCursorLeafStamps;

extern void trees_cursor_ArcSliceU8_drop(StdArcSlice *self);
extern void trees_cursor_ArcStamps_drop(StdArcInner *self);
extern bool trees_cursor_KeptCursor_resume(TreesCursorKeptCursor *self, HostIoIo *io, TreesPinPin *pin,
										   TreesCursorCursor *result, ChunksErrorError *error);
extern void trees_cursor_KeptCursor_drop(TreesCursorKeptCursor *self);
extern void trees_cursor_Cursor_keep(TreesCursorCursor *self, TreesCursorKeptCursor *result);
extern bool trees_cursor_Cursor_seek(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, const uint8 *key,
									 size_t key_len, const TreesKeyOrder *order, TreesCursorCursor *result,
									 ChunksErrorError *error);
extern bool trees_cursor_Cursor_seek_past(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, const uint8 *key,
										  size_t key_len, const TreesKeyOrder *order, TreesCursorCursor *result,
										  ChunksErrorError *error);
extern bool trees_cursor_Cursor_seek_node(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, const uint8 *key,
										  size_t key_len, uint8 level, const TreesKeyOrder *order,
										  TreesCursorCursor *result, ChunksErrorError *error);
extern bool trees_cursor_Cursor_first(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree,
									  TreesCursorCursor *result, ChunksErrorError *error);
extern bool trees_cursor_Cursor_first_with_read_ahead(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree,
													  size_t leaves, TreesCursorCursor *result,
													  ChunksErrorError *error);
extern void trees_cursor_Cursor_set_read_ahead(TreesCursorCursor *self, size_t leaves);
extern bool trees_cursor_Cursor_last(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, TreesCursorCursor *result,
									 ChunksErrorError *error);
extern bool trees_cursor_Cursor_at_ordinal(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, uint64 ordinal,
										   TreesCursorCursor *result, ChunksErrorError *error);
extern bool trees_cursor_Cursor_is_valid(const TreesCursorCursor *self);
extern TreesNodeNode trees_cursor_Cursor_node(const TreesCursorCursor *self);
extern StdArcSlice trees_cursor_Cursor_shared(TreesCursorCursor *self);
extern TreesStoreTree trees_cursor_Cursor_node_tree(const TreesCursorCursor *self);
extern size_t trees_cursor_Cursor_index(const TreesCursorCursor *self);
extern StdBytes trees_cursor_Cursor_key(const TreesCursorCursor *self);
extern StdBytes trees_cursor_Cursor_value(const TreesCursorCursor *self);
extern TreesStoreTree trees_cursor_Cursor_child_tree(const TreesCursorCursor *self);
extern bool trees_cursor_Cursor_stamps(const TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, StdVec *result,
									   ChunksErrorError *error);
extern bool trees_cursor_Cursor_values(const TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, StdVec *result,
									   ChunksErrorError *error);
extern bool trees_cursor_Cursor_at_node_end(const TreesCursorCursor *self);
extern bool trees_cursor_Cursor_tree_at(const TreesCursorCursor *self, uint8 level, TreesStoreTree *result);
extern bool trees_cursor_Cursor_starts_node(const TreesCursorCursor *self, uint8 level);
extern bool trees_cursor_Cursor_ordinal(const TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin,
										uint64 *result, ChunksErrorError *error);
extern bool trees_cursor_Cursor_next_in_node(TreesCursorCursor *self);
extern bool trees_cursor_Cursor_prev_in_node(TreesCursorCursor *self);
extern bool trees_cursor_Cursor_next(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin,
									 ChunksErrorError *error);
extern bool trees_cursor_Cursor_advance_to(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, const uint8 *key,
										   size_t key_len, const TreesKeyOrder *order, ChunksErrorError *error);
extern bool trees_cursor_Cursor_advance_to_node(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin,
												const uint8 *key, size_t key_len, const TreesKeyOrder *order,
												ChunksErrorError *error);
extern bool trees_cursor_Cursor_skip_node(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin, uint8 level,
										  ChunksErrorError *error);
extern bool trees_cursor_Cursor_prev(TreesCursorCursor *self, HostIoIo *io, TreesPinPin *pin,
									 ChunksErrorError *error);
extern ChunksTrailerLocator trees_cursor_Cursor_locator(const TreesCursorCursor *self);
extern void trees_cursor_Cursor_drop(TreesCursorCursor *self);
extern TreesCursorLeafStamps trees_cursor_LeafStamps_Default_default(void);
extern bool trees_cursor_LeafStamps_of(TreesCursorLeafStamps *self, HostIoIo *io, TreesPinPin *pin,
									   const TreesCursorCursor *cursor, StdVec *result, ChunksErrorError *error);
extern void trees_cursor_LeafStamps_drop(TreesCursorLeafStamps *self);

#endif							/* DOLTGRES_TREES_CURSOR_H */
