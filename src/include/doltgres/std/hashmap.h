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
 * The C form of std's HashMap and HashSet (the port text's section 9): a
 * port of hashbrown 0.17.1's RawTable, whose elements are a key and a value
 * of the sizes a map's type gives, hashed by the map's BuildHasher.
 */
#ifndef DOLTGRES_STD_HASHMAP_H
#define DOLTGRES_STD_HASHMAP_H

#include "doltgres/std/foldhash.h"
#include "doltgres/std/hash.h"

/*
 * StdRawTableInner is hashbrown's RawTableInner: the control bytes, which
 * the elements end at, the buckets less one, the count of elements, and
 * how many more fit before the table grows.
 */
typedef struct StdRawTableInner
{
	uint8	   *ctrl;
	size_t		bucket_mask;
	size_t		items;
	size_t		growth_left;
} StdRawTableInner;

/*
 * StdHashMapType is a map's generic parameters as data: its element's size
 * and alignment and where its value starts, the key's Hash, Eq, and drop,
 * the value's drop, and the BuildHasher S with S's Default. A set's value
 * is ().
 */
typedef struct StdHashMapType
{
	size_t		size;
	size_t		align;
	size_t		value_offset;
	void		(*key_hash) (const void *key, StdHasher state);
	bool		(*key_eq) (const void *a, const void *b);
	void		(*key_drop) (void *key);
	void		(*value_drop) (void *value);
	const StdBuildHasherOps *hasher;
	void		(*hasher_default) (void *result);
} StdHashMapType;

/* StdHashBuilder is a map's BuildHasher, one of the port's two. */
typedef union StdHashBuilder
{
	StdRandomState std;
	FoldhashFastRandomState fast;
} StdHashBuilder;

/* StdHashMap is std's HashMap over hashbrown's: its table and its BuildHasher. */
typedef struct StdHashMap
{
	StdRawTableInner table;
	StdHashBuilder hash_builder;
} StdHashMap;

/* StdHashMapEntry is rustc_entry's result: the element of the key, or the key's hash where it goes. */
/*
 * StdHashMapIter is hashbrown's RawIter: the group it is at, the full buckets
 * of that group still to come, and the elements left.
 */
typedef struct StdHashMapIter
{
	size_t		base;
	uint64		bits;
	size_t		items;
} StdHashMapIter;

/* StdHashMapIntoIter is hashbrown's RawIntoIter: the table it took, and an iterator over it. */
typedef struct StdHashMapIntoIter
{
	StdRawTableInner table;
	StdHashMapIter iter;
} StdHashMapIntoIter;

typedef struct StdHashMapEntry
{
	bool		occupied;
	void	   *elem;
	uint64		hash;
} StdHashMapEntry;

extern void std_HashMap_with_hasher(StdHashMap *map, const StdHashBuilder *hash_builder);
extern void std_HashMap_Default_default(StdHashMap *map, const StdHashMapType *t);
extern size_t std_HashMap_len(const StdHashMap *map);
extern bool std_HashMap_is_empty(const StdHashMap *map);
extern void std_HashMap_reserve(StdHashMap *map, const StdHashMapType *t, size_t additional);
extern void *std_HashMap_get(const StdHashMap *map, const StdHashMapType *t, const void *key);
extern bool std_HashMap_contains_key(const StdHashMap *map, const StdHashMapType *t, const void *key);
extern bool std_HashMap_insert(StdHashMap *map, const StdHashMapType *t, void *key, void *value,
							   void *result);
extern bool std_HashMap_remove(StdHashMap *map, const StdHashMapType *t, const void *key,
							   void *result);
extern StdHashMapEntry std_HashMap_entry(StdHashMap *map, const StdHashMapType *t, const void *key);
extern void *std_HashMap_VacantEntry_insert(StdHashMap *map, const StdHashMapType *t,
											const StdHashMapEntry *entry, void *key, void *value);
extern void std_HashMap_clear(StdHashMap *map, const StdHashMapType *t);
extern StdHashMapIter std_HashMap_Iter_new(const StdHashMap *map);
extern void *std_HashMap_Iter_next(StdHashMapIter *self, const StdRawTableInner *table, const StdHashMapType *t);
extern StdHashMapIntoIter std_HashMap_IntoIter_new(StdHashMap *map);
extern bool std_HashMap_IntoIter_next(StdHashMapIntoIter *self, const StdHashMapType *t, void *result);
extern void std_HashMap_IntoIter_drop(StdHashMapIntoIter *self, const StdHashMapType *t);
extern bool std_HashMap_PartialEq_eq(const StdHashMap *map, const StdHashMap *other,
									 const StdHashMapType *t,
									 bool (*value_eq) (const void *a, const void *b));
extern void std_HashMap_drop(StdHashMap *map, const StdHashMapType *t);

#endif							/* DOLTGRES_STD_HASHMAP_H */
