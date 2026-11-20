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
 * Pin scopes (trees' pin.rs): the scope in which tree code reads nodes from
 * a store, each node read once, and the walk that reads a batch of entries a
 * scope at a time.
 */
#ifndef DOLTGRES_TREES_PIN_H
#define DOLTGRES_TREES_PIN_H

#include "doltgres/std/frozen_map.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/store.h"

struct TreesCursorCursor;

/*
 * TreesPinPin is Pin: a scope in which tree code reads nodes from a store,
 * and the nodes it read, a FrozenMap<Tree, Box<Flat>, AddressState> by the
 * tree name that named each.
 */
typedef struct TreesPinPin
{
	TreesStoreNodeStore store;
	StdFrozenMap nodes;
} TreesPinPin;

extern const StdHashMapType TREES_PIN_NODES_TYPE;
extern bool trees_pin_walk(HostIoIo *io, TreesStoreNodeStore store, struct TreesCursorCursor *cursor,
						   bool (*f) (void *captures, HostIoIo *io, TreesPinPin *pin,
									  const struct TreesCursorCursor *cursor, bool *result, ChunksErrorError *error),
						   void *f_captures, void (*f_drop) (void *), bool *result, ChunksErrorError *error);
extern bool trees_pin_Pin_node(TreesPinPin *self, HostIoIo *io, TreesStoreNamed tree, TreesNodeNode *result,
							   ChunksErrorError *error);
extern bool trees_pin_Pin_own(TreesPinPin *self, HostIoIo *io, TreesStoreNamed tree, TreesNodeFlat *result,
							  ChunksErrorError *error);
extern bool trees_pin_Pin_prefetch(TreesPinPin *self, HostIoIo *io, TreesStoreNamed tree, ChunksErrorError *error);
extern bool trees_pin_Pin_stamps(TreesPinPin *self, HostIoIo *io, TreesStoreNamed tree, StdArcInner **result,
								 ChunksErrorError *error);

#endif							/* DOLTGRES_TREES_PIN_H */
