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
 * The storage crates' hash map and set, and their hashers: the standard
 * library's and foldhash's random ones. The map and the set give lookups,
 * inserts, and removals by key and are walked only in key order, so the order
 * a hash gives, which differs from process to process, reaches no stored
 * byte, WAL record, or result through any of their users (chunks' hash.rs).
 * Their functions are hash_template.h's, an instance per key and value type.
 */
#ifndef DOLTGRES_CHUNKS_HASH_H
#define DOLTGRES_CHUNKS_HASH_H

#include "doltgres/std/hashmap.h"

/*
 * ChunksHashHashMap is the storage crates' hash map, walked only in key
 * order, with the standard hasher unless it names another.
 */
typedef struct ChunksHashHashMap
{
	StdHashMap	f0;
} ChunksHashHashMap;

/*
 * ChunksHashHashSet is the storage crates' hash set, walked only in order,
 * with the standard hasher unless it names another.
 */
typedef struct ChunksHashHashSet
{
	StdHashMap	f0;
} ChunksHashHashSet;

extern void test_chunks_hash_tests_a_failed_into_iter_drops_every_entry(void);
extern void test_chunks_hash_tests_a_failed_into_iter_drops_every_member(void);
extern void test_chunks_hash_tests_a_map_is_walked_in_key_order(void);
extern void test_chunks_hash_tests_a_set_is_walked_in_order(void);

#endif							/* DOLTGRES_CHUNKS_HASH_H */
