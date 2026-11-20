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
 * elsa 1.11.2's FrozenMap (the port text's section 15): an append-only hash
 * map whose insert and get take it shared, a std HashMap of keys to
 * separately allocated values behind a flag that refuses a reentrant use,
 * whose values' targets stay put until the map drops.
 */
#ifndef DOLTGRES_STD_FROZEN_MAP_H
#define DOLTGRES_STD_FROZEN_MAP_H

#include "doltgres/std/any.h"
#include "doltgres/std/hashmap.h"

/* StdFrozenMap is FrozenMap<K, V, S>: its map and the flag set while one of its methods runs. */
typedef struct StdFrozenMap
{
	StdHashMap	map;
	bool		in_use;
} StdFrozenMap;

extern void std_FrozenMap_Default_default(StdFrozenMap *result, const StdHashMapType *t);
extern const void *std_FrozenMap_insert(StdFrozenMap *self, const StdHashMapType *t, const StdDerefOps *deref,
										void *k, void *v);
extern const void *std_FrozenMap_get(StdFrozenMap *self, const StdHashMapType *t, const StdDerefOps *deref,
									 const void *k);
extern void std_FrozenMap_drop(StdFrozenMap *self, const StdHashMapType *t);

#endif							/* DOLTGRES_STD_FROZEN_MAP_H */
