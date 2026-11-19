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
 * The template of chunks' hash.rs (the port text's section 2): HashMap<K, V,
 * S>'s instance at its key, value, and hasher, or HashSet<T, S>'s at its
 * member and hasher, each over std's map with the instance's StdHashMapType,
 * with the sorts, extend, and from_iter instances it needs. An instance
 * defines its parameters and includes this file; every parameter is
 * undefined at the end.
 *
 * CHUNKS_HASH_SCOPE is static for an instance one file alone uses, and
 * extern for one defined in one file and declared in others.
 * CHUNKS_HASH_DECLARE generates the instance's types and declarations, and
 * CHUNKS_HASH_DEFINE its definitions, which its declarations precede.
 * CHUNKS_HASH_TS spells the instance's type arguments, CHUNKS_HASH_TYPE is
 * its map's StdHashMapType, and CHUNKS_HASH_ENTRY_FREEZE, _COPY, and
 * _BRANCHLESS are the sort template's facts of an owned entry.
 *
 * HashMap: CHUNKS_HASH_K and CHUNKS_HASH_V are K and V, CHUNKS_HASH_ENTRY
 * is the (K, V) tuple of the map's element layout, CHUNKS_HASH_K_CMP is K's
 * Ord::cmp on pointers, CHUNKS_HASH_V_EQ V's PartialEq::eq, and
 * CHUNKS_HASH_K_DEBUG and CHUNKS_HASH_V_DEBUG their Debug.
 *
 * HashSet: CHUNKS_HASH_SET is defined, CHUNKS_HASH_T is T, CHUNKS_HASH_T_LT
 * is T's PartialOrd::lt as a StdIsLess, and CHUNKS_HASH_T_DEBUG its Debug.
 */

#include "doltgres/chunks/hash.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"

/* CHUNKS_HASH_CAT pastes two tokens after expanding them. */
#define CHUNKS_HASH_CAT_(a, b) a##b
#define CHUNKS_HASH_CAT(a, b) CHUNKS_HASH_CAT_(a, b)

/* CHUNKS_HASH_DEF is CHUNKS_HASH_SCOPE's storage class for a definition: static, or none for extern. */
#define CHUNKS_HASH_DEF_static static
#define CHUNKS_HASH_DEF_extern
#define CHUNKS_HASH_DEF CHUNKS_HASH_CAT(CHUNKS_HASH_DEF_, CHUNKS_HASH_SCOPE)

/* CHUNKS_HASH_RUST_FILE is hash.rs, where the template's panics are. */
#define CHUNKS_HASH_RUST_FILE "chunks" STD_SEP "src" STD_SEP "hash.rs"

#ifndef CHUNKS_HASH_SET

/* CHUNKS_HASH_NAME names the map instance's functions and types. */
#define CHUNKS_HASH_NAME(name) CHUNKS_HASH_CAT(CHUNKS_HASH_CAT(chunks_hash_HashMap, CHUNKS_HASH_TS), _##name)

/* CHUNKS_HASH_REF_NAME names the instance's &HashMap's functions. */
#define CHUNKS_HASH_REF_NAME(name) CHUNKS_HASH_CAT(CHUNKS_HASH_CAT(chunks_hash_RefHashMap, CHUNKS_HASH_TS), _##name)

/* CHUNKS_HASH_PAIR is iter's element, a (&K, &V). */
#define CHUNKS_HASH_PAIR CHUNKS_HASH_NAME(iter_t)

#ifdef CHUNKS_HASH_DECLARE

/* The instance's iter's element, an entry's key and value by reference. */
typedef struct CHUNKS_HASH_PAIR
{
	const		CHUNKS_HASH_K *f0;
	const		CHUNKS_HASH_V *f1;
}			CHUNKS_HASH_PAIR;

/* The instance's keys' iterator: iter's, each entry's key. */
typedef struct CHUNKS_HASH_NAME (keys_iter)
{
	StdVecIntoIter inner;
} CHUNKS_HASH_NAME(keys_iter);

/* The instance's values' iterator: iter's, each entry's value. */
typedef struct CHUNKS_HASH_NAME (values_iter)
{
	StdVecIntoIter inner;
} CHUNKS_HASH_NAME(values_iter);

CHUNKS_HASH_SCOPE size_t CHUNKS_HASH_NAME(len) (const ChunksHashHashMap *self);
CHUNKS_HASH_SCOPE const CHUNKS_HASH_V *CHUNKS_HASH_NAME(get) (const ChunksHashHashMap *self, const CHUNKS_HASH_K * key);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(Default_default) (ChunksHashHashMap *result);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(is_empty) (const ChunksHashHashMap *self);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(contains_key) (const ChunksHashHashMap *self, const CHUNKS_HASH_K * key);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(insert) (ChunksHashHashMap *self, CHUNKS_HASH_K * key, CHUNKS_HASH_V * value,
												 CHUNKS_HASH_V * result);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(remove) (ChunksHashHashMap *self, const CHUNKS_HASH_K * key,
												 CHUNKS_HASH_V * result);
CHUNKS_HASH_SCOPE StdHashMapEntry CHUNKS_HASH_NAME(entry) (ChunksHashHashMap *self, const CHUNKS_HASH_K * key);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(clear) (ChunksHashHashMap *self);
CHUNKS_HASH_SCOPE const CHUNKS_HASH_V *CHUNKS_HASH_NAME(Index_index) (const ChunksHashHashMap *self,
																	  const CHUNKS_HASH_K * key);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(PartialEq_eq) (const ChunksHashHashMap *self,
													   const ChunksHashHashMap *other);
CHUNKS_HASH_SCOPE StdVecIntoIter CHUNKS_HASH_NAME(iter) (const ChunksHashHashMap *self);
CHUNKS_HASH_SCOPE CHUNKS_HASH_NAME(keys_iter) CHUNKS_HASH_NAME(keys) (const ChunksHashHashMap *self);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(keys_iter_next) (CHUNKS_HASH_NAME(keys_iter) * self,
														 const CHUNKS_HASH_K * *result);
CHUNKS_HASH_SCOPE CHUNKS_HASH_NAME(values_iter) CHUNKS_HASH_NAME(values) (const ChunksHashHashMap *self);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(values_iter_next) (CHUNKS_HASH_NAME(values_iter) * self,
														   const CHUNKS_HASH_V * *result);
CHUNKS_HASH_SCOPE StdVecIntoIter CHUNKS_HASH_NAME(IntoIterator_into_iter) (ChunksHashHashMap *self);
CHUNKS_HASH_SCOPE StdVecIntoIter CHUNKS_HASH_REF_NAME(IntoIterator_into_iter) (const ChunksHashHashMap *self);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(FromIterator_from_iter) (StdIterator entries, ChunksHashHashMap *result);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(Extend_extend) (ChunksHashHashMap *self, StdIterator entries);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(Debug_fmt) (const void *value, StdFormatter *f);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(drop) (ChunksHashHashMap *self);

#endif							/* CHUNKS_HASH_DECLARE */

#ifdef CHUNKS_HASH_DEFINE

/* The ipnsort instance at iter's elements, which sort_unstable_by sorts by key. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS CHUNKS_HASH_NAME(iter_t)
#define STD_SORT_T CHUNKS_HASH_PAIR
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_WITH_BY
#define STD_SORT_WITHOUT_UNSTABLE
#include "doltgres/std/sort_template.h"

/* The ipnsort instance at owned entries, which into_iter's sort_unstable_by sorts by key. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS CHUNKS_HASH_NAME(entry_t)
#define STD_SORT_T CHUNKS_HASH_ENTRY
#define STD_SORT_FREEZE CHUNKS_HASH_ENTRY_FREEZE
#define STD_SORT_COPY CHUNKS_HASH_ENTRY_COPY
#define STD_SORT_BRANCHLESS CHUNKS_HASH_ENTRY_BRANCHLESS
#define STD_SORT_WITH_BY
#define STD_SORT_WITHOUT_UNSTABLE
#include "doltgres/std/sort_template.h"

/* The std HashMap instance at owned entries, which from_iter and extend take. */
#define STD_HASHMAP_SCOPE static
#define STD_HASHMAP_DECLARE
#define STD_HASHMAP_DEFINE
#define STD_HASHMAP_TS CHUNKS_HASH_NAME(entry_t)
#define STD_HASHMAP_ITEM CHUNKS_HASH_ENTRY
#define STD_HASHMAP_V CHUNKS_HASH_V
#define STD_HASHMAP_TYPE CHUNKS_HASH_TYPE
#include "doltgres/std/hashmap_template.h"

/* The instance's len returns the number of entries. */
CHUNKS_HASH_DEF size_t
CHUNKS_HASH_NAME(len) (const ChunksHashHashMap *self)
{
	return std_HashMap_len(&self->f0);
}

/* The instance's get returns a key's value. */
CHUNKS_HASH_DEF const CHUNKS_HASH_V *
CHUNKS_HASH_NAME(get) (const ChunksHashHashMap *self, const CHUNKS_HASH_K * key)
{
	const uint8 *elem = std_HashMap_get(&self->f0, CHUNKS_HASH_TYPE, key);

	return elem == NULL ? NULL : (const CHUNKS_HASH_V *) (elem + offsetof(CHUNKS_HASH_ENTRY, f1));
}

/* The instance's Default is an empty map of S's default hasher. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(Default_default) (ChunksHashHashMap *result)
{
	std_HashMap_Default_default(&result->f0, CHUNKS_HASH_TYPE);
}

/* The instance's is_empty reports whether the map holds no entry. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(is_empty) (const ChunksHashHashMap *self)
{
	return std_HashMap_is_empty(&self->f0);
}

/* The instance's contains_key reports whether the map holds a key. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(contains_key) (const ChunksHashHashMap *self, const CHUNKS_HASH_K * key)
{
	return std_HashMap_contains_key(&self->f0, CHUNKS_HASH_TYPE, key);
}

/* The instance's insert sets a key's value and returns the one it had. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(insert) (ChunksHashHashMap *self, CHUNKS_HASH_K * key, CHUNKS_HASH_V * value, CHUNKS_HASH_V * result)
{
	return std_HashMap_insert(&self->f0, CHUNKS_HASH_TYPE, key, value, result);
}

/* The instance's remove takes a key's entry out and returns its value. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(remove) (ChunksHashHashMap *self, const CHUNKS_HASH_K * key, CHUNKS_HASH_V * result)
{
	return std_HashMap_remove(&self->f0, CHUNKS_HASH_TYPE, key, result);
}

/* The instance's entry returns a key's place in the map, to fill or change. */
CHUNKS_HASH_DEF StdHashMapEntry
CHUNKS_HASH_NAME(entry) (ChunksHashHashMap *self, const CHUNKS_HASH_K * key)
{
	return std_HashMap_entry(&self->f0, CHUNKS_HASH_TYPE, key);
}

/* The instance's clear empties the map. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(clear) (ChunksHashHashMap *self)
{
	std_HashMap_clear(&self->f0, CHUNKS_HASH_TYPE);
}

/*
 * The instance's Index is std's HashMap Index, its get's value, panicking
 * at the indexing expression for a key the map lacks.
 */
CHUNKS_HASH_DEF const CHUNKS_HASH_V *
CHUNKS_HASH_NAME(Index_index) (const ChunksHashHashMap *self, const CHUNKS_HASH_K * key)
{
	static const char text[] = "no entry found for key";
	const uint8 *elem = std_HashMap_get(&self->f0, CHUNKS_HASH_TYPE, key);

	if (elem == NULL)
		std_option_expect_failed(text, sizeof(text) - 1, CHUNKS_HASH_RUST_FILE, 141, 16);
	return (const CHUNKS_HASH_V *) (elem + offsetof(CHUNKS_HASH_ENTRY, f1));
}

/* The instance's PartialEq is std's map's: the same keys with equal values. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(PartialEq_eq) (const ChunksHashHashMap *self, const ChunksHashHashMap *other)
{
	return std_HashMap_PartialEq_eq(&self->f0, &other->f0, CHUNKS_HASH_TYPE, CHUNKS_HASH_V_EQ);
}

/* The instance's iter_closure_0 is iter's comparison, the entries' keys' order. */
static StdOrdering
CHUNKS_HASH_NAME(iter_closure_0) (void *captures, const void *a, const void *b)
{
	return CHUNKS_HASH_K_CMP(((const CHUNKS_HASH_PAIR *) a)->f0, ((const CHUNKS_HASH_PAIR *) b)->f0);
}

/* The instance's iter walks the entries in key order. */
CHUNKS_HASH_DEF StdVecIntoIter
CHUNKS_HASH_NAME(iter) (const ChunksHashHashMap *self)
{
	volatile StdVec entries;
	volatile bool entries_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *entries_v = (StdVec *) &entries;
		StdHashMapIter iter = std_HashMap_Iter_new(&self->f0);
		const uint8 *elem = std_HashMap_Iter_next(&iter, &self->f0.table, CHUNKS_HASH_TYPE);
		StdVecIntoIter result;

		if (elem == NULL)
			std_vec_new(entries_v, sizeof(CHUNKS_HASH_PAIR), _Alignof(CHUNKS_HASH_PAIR));
		else
		{
			CHUNKS_HASH_PAIR pair = {(const CHUNKS_HASH_K *) elem,
			(const CHUNKS_HASH_V *) (elem + offsetof(CHUNKS_HASH_ENTRY, f1))};

			std_vec_collect_first(entries_v, &pair, iter.items, sizeof(CHUNKS_HASH_PAIR), _Alignof(CHUNKS_HASH_PAIR));
			entries_live = true;
			while ((elem = std_HashMap_Iter_next(&iter, &self->f0.table, CHUNKS_HASH_TYPE)) != NULL)
			{
				pair.f0 = (const CHUNKS_HASH_K *) elem;
				pair.f1 = (const CHUNKS_HASH_V *) (elem + offsetof(CHUNKS_HASH_ENTRY, f1));
				std_vec_extend_one(entries_v, &pair, iter.items, sizeof(CHUNKS_HASH_PAIR),
								   _Alignof(CHUNKS_HASH_PAIR));
			}
		}
		entries_live = true;
		CHUNKS_HASH_CAT(std_slice_sort_unstable_by_, CHUNKS_HASH_NAME(iter_t)) (entries_v->ptr, entries_v->len,
																				CHUNKS_HASH_NAME(iter_closure_0), NULL, NULL);
		entries_live = false;
		result = std_Vec_IntoIterator_into_iter(entries_v, sizeof(CHUNKS_HASH_PAIR), _Alignof(CHUNKS_HASH_PAIR), NULL);
		std_unwind_pop(&unwind);
		return result;
	}
	else
	{
		if (entries_live)
			std_vec_free_Checked((StdVec *) &entries, sizeof(CHUNKS_HASH_PAIR), _Alignof(CHUNKS_HASH_PAIR));
		std_unwind_resume(&unwind);
	}
	pg_unreachable();
}

/* The instance's keys walks the keys in order. */
CHUNKS_HASH_DEF
CHUNKS_HASH_NAME(keys_iter)
CHUNKS_HASH_NAME(keys) (const ChunksHashHashMap *self)
{
	CHUNKS_HASH_NAME(keys_iter) keys;

	keys.inner = CHUNKS_HASH_NAME(iter) (self);
	return keys;
}

/* The instance's keys_iter_next is keys' next: the next entry's key. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(keys_iter_next) (CHUNKS_HASH_NAME(keys_iter) * self, const CHUNKS_HASH_K * *result)
{
	CHUNKS_HASH_PAIR pair;

	if (!std_VecIntoIter_Iterator_next(&self->inner, &pair))
		return false;
	*result = pair.f0;
	return true;
}

/* The instance's values walks the values in their keys' order. */
CHUNKS_HASH_DEF
CHUNKS_HASH_NAME(values_iter)
CHUNKS_HASH_NAME(values) (const ChunksHashHashMap *self)
{
	CHUNKS_HASH_NAME(values_iter) values;

	values.inner = CHUNKS_HASH_NAME(iter) (self);
	return values;
}

/* The instance's values_iter_next is values' next: the next entry's value. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(values_iter_next) (CHUNKS_HASH_NAME(values_iter) * self, const CHUNKS_HASH_V * *result)
{
	CHUNKS_HASH_PAIR pair;

	if (!std_VecIntoIter_Iterator_next(&self->inner, &pair))
		return false;
	*result = pair.f1;
	return true;
}

/* The instance's into_iter_closure_0 is into_iter's comparison, the entries' keys' order. */
static StdOrdering
CHUNKS_HASH_NAME(IntoIterator_into_iter_closure_0) (void *captures, const void *a, const void *b)
{
	return CHUNKS_HASH_K_CMP(&((const CHUNKS_HASH_ENTRY *) a)->f0, &((const CHUNKS_HASH_ENTRY *) b)->f0);
}

/* The instance's entry_drop is the drop glue of an owned entry, its key's and then its value's. */
static void
CHUNKS_HASH_NAME(entry_drop) (void *elem)
{
	const StdHashMapType *t = CHUNKS_HASH_TYPE;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		if (t->key_drop != NULL)
			t->key_drop(elem);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (t->value_drop != NULL)
			t->value_drop((uint8 *) elem + offsetof(CHUNKS_HASH_ENTRY, f1));
		std_unwind_resume(&unwind);
	}
	if (t->value_drop != NULL)
		t->value_drop((uint8 *) elem + offsetof(CHUNKS_HASH_ENTRY, f1));
}

/* The instance's IntoIterator takes the map's entries in key order. */
CHUNKS_HASH_DEF StdVecIntoIter
CHUNKS_HASH_NAME(IntoIterator_into_iter) (ChunksHashHashMap *self)
{
	volatile StdHashMapIntoIter iter;
	volatile bool iter_live = false;
	volatile StdVec entries;
	volatile bool entries_live = false;
	volatile	CHUNKS_HASH_ENTRY entry;
	volatile bool entry_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdHashMapIntoIter *iter_v = (StdHashMapIntoIter *) &iter;
		StdVec	   *entries_v = (StdVec *) &entries;
		CHUNKS_HASH_ENTRY *entry_v = (CHUNKS_HASH_ENTRY *) & entry;
		StdVecIntoIter result;

		*iter_v = std_HashMap_IntoIter_new(&self->f0);
		iter_live = true;
		if (!std_HashMap_IntoIter_next(iter_v, CHUNKS_HASH_TYPE, entry_v))
			std_vec_new(entries_v, sizeof(CHUNKS_HASH_ENTRY), _Alignof(CHUNKS_HASH_ENTRY));
		else
		{
			entry_live = true;
			std_vec_collect_first(entries_v, entry_v, iter_v->iter.items, sizeof(CHUNKS_HASH_ENTRY),
								  _Alignof(CHUNKS_HASH_ENTRY));
			entry_live = false;
			entries_live = true;
			while (std_HashMap_IntoIter_next(iter_v, CHUNKS_HASH_TYPE, entry_v))
			{
				entry_live = true;
				std_vec_extend_one(entries_v, entry_v, iter_v->iter.items, sizeof(CHUNKS_HASH_ENTRY),
								   _Alignof(CHUNKS_HASH_ENTRY));
				entry_live = false;
			}
		}
		entries_live = true;
		iter_live = false;
		std_HashMap_IntoIter_drop(iter_v, CHUNKS_HASH_TYPE);
		CHUNKS_HASH_CAT(std_slice_sort_unstable_by_, CHUNKS_HASH_NAME(entry_t)) (entries_v->ptr, entries_v->len,
																				 CHUNKS_HASH_NAME(IntoIterator_into_iter_closure_0),
																				 NULL, NULL);
		entries_live = false;
		result = std_Vec_IntoIterator_into_iter(entries_v, sizeof(CHUNKS_HASH_ENTRY), _Alignof(CHUNKS_HASH_ENTRY),
												CHUNKS_HASH_NAME(entry_drop));
		std_unwind_pop(&unwind);
		return result;
	}
	else
	{
		if (entry_live)
			CHUNKS_HASH_NAME(entry_drop) ((void *) &entry);
		if (entries_live)
			std_vec_drop_Checked((StdVec *) &entries, sizeof(CHUNKS_HASH_ENTRY), _Alignof(CHUNKS_HASH_ENTRY),
								 CHUNKS_HASH_NAME(entry_drop));
		if (iter_live)
			std_HashMap_IntoIter_drop((StdHashMapIntoIter *) &iter, CHUNKS_HASH_TYPE);
		std_unwind_resume(&unwind);
	}
	pg_unreachable();
}

/* The instance's &HashMap IntoIterator is iter. */
CHUNKS_HASH_DEF StdVecIntoIter
CHUNKS_HASH_REF_NAME(IntoIterator_into_iter) (const ChunksHashHashMap *self)
{
	return CHUNKS_HASH_NAME(iter) (self);
}

/* The instance's FromIterator is std's map's, of S's default hasher. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(FromIterator_from_iter) (StdIterator entries, ChunksHashHashMap *result)
{
	CHUNKS_HASH_CAT(std_HashMap_from_iter_, CHUNKS_HASH_NAME(entry_t)) (entries, &result->f0);
}

/* The instance's Extend is std's map's. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(Extend_extend) (ChunksHashHashMap *self, StdIterator entries)
{
	CHUNKS_HASH_CAT(std_HashMap_extend_, CHUNKS_HASH_NAME(entry_t)) (&self->f0, entries);
}

/* The instance's Debug writes the entries in key order as a map. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(Debug_fmt) (const void *value, StdFormatter *f)
{
	const ChunksHashHashMap *self = value;
	StdDebugMap map = std_fmt_debug_map(f);
	volatile StdVecIntoIter entries;
	volatile bool entries_live = false;
	bool		result;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVecIntoIter *entries_v = (StdVecIntoIter *) &entries;
		CHUNKS_HASH_PAIR pair;

		*entries_v = CHUNKS_HASH_NAME(iter) (self);
		entries_live = true;
		while (std_VecIntoIter_Iterator_next(entries_v, &pair))
			std_DebugMap_entry(&map, pair.f0, CHUNKS_HASH_K_DEBUG, pair.f1, CHUNKS_HASH_V_DEBUG);
		entries_live = false;
		std_VecIntoIter_drop(entries_v);
		result = std_DebugMap_finish(&map);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (entries_live)
			std_VecIntoIter_drop((StdVecIntoIter *) &entries);
		std_unwind_resume(&unwind);
	}
	return result;
}

/* The instance's drop is the map's drop glue, std's map's. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(drop) (ChunksHashHashMap *self)
{
	std_HashMap_drop(&self->f0, CHUNKS_HASH_TYPE);
}

#endif							/* CHUNKS_HASH_DEFINE */

#undef CHUNKS_HASH_NAME
#undef CHUNKS_HASH_REF_NAME
#undef CHUNKS_HASH_PAIR

#else							/* CHUNKS_HASH_SET */

/* CHUNKS_HASH_NAME names the set instance's functions. */
#define CHUNKS_HASH_NAME(name) CHUNKS_HASH_CAT(CHUNKS_HASH_CAT(chunks_hash_HashSet, CHUNKS_HASH_TS), _##name)

/* CHUNKS_HASH_REF_NAME names the instance's &HashSet's functions. */
#define CHUNKS_HASH_REF_NAME(name) CHUNKS_HASH_CAT(CHUNKS_HASH_CAT(chunks_hash_RefHashSet, CHUNKS_HASH_TS), _##name)

#ifdef CHUNKS_HASH_DECLARE

CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(Default_default) (ChunksHashHashSet *result);
CHUNKS_HASH_SCOPE size_t CHUNKS_HASH_NAME(len) (const ChunksHashHashSet *self);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(is_empty) (const ChunksHashHashSet *self);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(contains) (const ChunksHashHashSet *self, const CHUNKS_HASH_T * value);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(insert) (ChunksHashHashSet *self, CHUNKS_HASH_T * value);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(remove) (ChunksHashHashSet *self, const CHUNKS_HASH_T * value);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(clear) (ChunksHashHashSet *self);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(PartialEq_eq) (const ChunksHashHashSet *self,
													   const ChunksHashHashSet *other);
CHUNKS_HASH_SCOPE StdVecIntoIter CHUNKS_HASH_NAME(iter) (const ChunksHashHashSet *self);
CHUNKS_HASH_SCOPE StdVecIntoIter CHUNKS_HASH_NAME(IntoIterator_into_iter) (ChunksHashHashSet *self);
CHUNKS_HASH_SCOPE StdVecIntoIter CHUNKS_HASH_REF_NAME(IntoIterator_into_iter) (const ChunksHashHashSet *self);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(FromIterator_from_iter) (StdIterator members, ChunksHashHashSet *result);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(Extend_extend) (ChunksHashHashSet *self, StdIterator members);
CHUNKS_HASH_SCOPE bool CHUNKS_HASH_NAME(Debug_fmt) (const void *value, StdFormatter *f);
CHUNKS_HASH_SCOPE void CHUNKS_HASH_NAME(drop) (ChunksHashHashSet *self);

#endif							/* CHUNKS_HASH_DECLARE */

#ifdef CHUNKS_HASH_DEFINE

/* The instance's iter's element, a member by reference. */
typedef const CHUNKS_HASH_T *CHUNKS_HASH_NAME(ref_t);

static bool CHUNKS_HASH_NAME(ref_lt) (void *captures, const void *a, const void *b);

/* The ipnsort instance at iter's members, references to T. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS CHUNKS_HASH_NAME(ref_t)
#define STD_SORT_T CHUNKS_HASH_NAME(ref_t)
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT CHUNKS_HASH_NAME(ref_lt)
#include "doltgres/std/sort_template.h"

/* The ipnsort instance at owned members. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS CHUNKS_HASH_NAME(t)
#define STD_SORT_T CHUNKS_HASH_T
#define STD_SORT_FREEZE CHUNKS_HASH_ENTRY_FREEZE
#define STD_SORT_COPY CHUNKS_HASH_ENTRY_COPY
#define STD_SORT_BRANCHLESS CHUNKS_HASH_ENTRY_BRANCHLESS
#define STD_SORT_LT CHUNKS_HASH_T_LT
#include "doltgres/std/sort_template.h"

/* The std HashMap instance at owned members, a set's (T, ()), which from_iter and extend take. */
#define STD_HASHMAP_SCOPE static
#define STD_HASHMAP_DECLARE
#define STD_HASHMAP_DEFINE
#define STD_HASHMAP_TS CHUNKS_HASH_NAME(t)
#define STD_HASHMAP_ITEM CHUNKS_HASH_T
#define STD_HASHMAP_UNIT
#define STD_HASHMAP_TYPE CHUNKS_HASH_TYPE
#include "doltgres/std/hashmap_template.h"

/* The instance's ref_lt is <&T as PartialOrd>::lt, T's lt through the references. */
static bool
CHUNKS_HASH_NAME(ref_lt) (void *captures, const void *a, const void *b)
{
	return CHUNKS_HASH_T_LT(captures, *(const CHUNKS_HASH_T * const *) a, *(const CHUNKS_HASH_T * const *) b);
}

/* The instance's Default is an empty set of S's default hasher. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(Default_default) (ChunksHashHashSet *result)
{
	std_HashMap_Default_default(&result->f0, CHUNKS_HASH_TYPE);
}

/* The instance's len returns the number of members. */
CHUNKS_HASH_DEF size_t
CHUNKS_HASH_NAME(len) (const ChunksHashHashSet *self)
{
	return std_HashMap_len(&self->f0);
}

/* The instance's is_empty reports whether the set holds no member. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(is_empty) (const ChunksHashHashSet *self)
{
	return std_HashMap_is_empty(&self->f0);
}

/* The instance's contains reports whether the set holds a value. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(contains) (const ChunksHashHashSet *self, const CHUNKS_HASH_T * value)
{
	return std_HashMap_contains_key(&self->f0, CHUNKS_HASH_TYPE, value);
}

/* The instance's insert adds a value and reports whether it was new. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(insert) (ChunksHashHashSet *self, CHUNKS_HASH_T * value)
{
	return !std_HashMap_insert(&self->f0, CHUNKS_HASH_TYPE, value, STD_DANGLING, STD_DANGLING);
}

/* The instance's remove takes a value out and reports whether the set held it. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(remove) (ChunksHashHashSet *self, const CHUNKS_HASH_T * value)
{
	return std_HashMap_remove(&self->f0, CHUNKS_HASH_TYPE, value, STD_DANGLING);
}

/* The instance's clear empties the set. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(clear) (ChunksHashHashSet *self)
{
	std_HashMap_clear(&self->f0, CHUNKS_HASH_TYPE);
}

/* The instance's PartialEq is std's set's: the same members. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(PartialEq_eq) (const ChunksHashHashSet *self, const ChunksHashHashSet *other)
{
	return std_HashMap_PartialEq_eq(&self->f0, &other->f0, CHUNKS_HASH_TYPE, NULL);
}

/* The instance's iter walks the members in order. */
CHUNKS_HASH_DEF StdVecIntoIter
CHUNKS_HASH_NAME(iter) (const ChunksHashHashSet *self)
{
	volatile StdVec members;
	volatile bool members_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *members_v = (StdVec *) &members;
		StdHashMapIter iter = std_HashMap_Iter_new(&self->f0);
		const		CHUNKS_HASH_T *member = std_HashMap_Iter_next(&iter, &self->f0.table, CHUNKS_HASH_TYPE);
		StdVecIntoIter result;

		if (member == NULL)
			std_vec_new(members_v, sizeof(const CHUNKS_HASH_T *), _Alignof(const CHUNKS_HASH_T *));
		else
		{
			std_vec_collect_first(members_v, &member, iter.items, sizeof(const CHUNKS_HASH_T *),
								  _Alignof(const CHUNKS_HASH_T *));
			members_live = true;
			while ((member = std_HashMap_Iter_next(&iter, &self->f0.table, CHUNKS_HASH_TYPE)) != NULL)
				std_vec_extend_one(members_v, &member, iter.items, sizeof(const CHUNKS_HASH_T *),
								   _Alignof(const CHUNKS_HASH_T *));
		}
		members_live = true;
		CHUNKS_HASH_CAT(std_slice_sort_unstable_, CHUNKS_HASH_NAME(ref_t)) (members_v->ptr, members_v->len);
		members_live = false;
		result = std_Vec_IntoIterator_into_iter(members_v, sizeof(const CHUNKS_HASH_T *),
												_Alignof(const CHUNKS_HASH_T *), NULL);
		std_unwind_pop(&unwind);
		return result;
	}
	else
	{
		if (members_live)
			std_vec_free_Checked((StdVec *) &members, sizeof(const CHUNKS_HASH_T *), _Alignof(const CHUNKS_HASH_T *));
		std_unwind_resume(&unwind);
	}
	pg_unreachable();
}

/* The instance's member_drop is an owned member's drop glue. */
static void
CHUNKS_HASH_NAME(member_drop) (void *member)
{
	if (CHUNKS_HASH_TYPE->key_drop != NULL)
		CHUNKS_HASH_TYPE->key_drop(member);
}

/* The instance's IntoIterator takes the set's members in order. */
CHUNKS_HASH_DEF StdVecIntoIter
CHUNKS_HASH_NAME(IntoIterator_into_iter) (ChunksHashHashSet *self)
{
	volatile StdHashMapIntoIter iter;
	volatile bool iter_live = false;
	volatile StdVec members;
	volatile bool members_live = false;
	volatile	CHUNKS_HASH_T member;
	volatile bool member_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdHashMapIntoIter *iter_v = (StdHashMapIntoIter *) &iter;
		StdVec	   *members_v = (StdVec *) &members;
		CHUNKS_HASH_T *member_v = (CHUNKS_HASH_T *) & member;
		StdVecIntoIter result;

		*iter_v = std_HashMap_IntoIter_new(&self->f0);
		iter_live = true;
		if (!std_HashMap_IntoIter_next(iter_v, CHUNKS_HASH_TYPE, member_v))
			std_vec_new(members_v, sizeof(CHUNKS_HASH_T), _Alignof(CHUNKS_HASH_T));
		else
		{
			member_live = true;
			std_vec_collect_first(members_v, member_v, iter_v->iter.items, sizeof(CHUNKS_HASH_T),
								  _Alignof(CHUNKS_HASH_T));
			member_live = false;
			members_live = true;
			while (std_HashMap_IntoIter_next(iter_v, CHUNKS_HASH_TYPE, member_v))
			{
				member_live = true;
				std_vec_extend_one(members_v, member_v, iter_v->iter.items, sizeof(CHUNKS_HASH_T),
								   _Alignof(CHUNKS_HASH_T));
				member_live = false;
			}
		}
		members_live = true;
		iter_live = false;
		std_HashMap_IntoIter_drop(iter_v, CHUNKS_HASH_TYPE);
		CHUNKS_HASH_CAT(std_slice_sort_unstable_, CHUNKS_HASH_NAME(t)) (members_v->ptr, members_v->len);
		members_live = false;
		result = std_Vec_IntoIterator_into_iter(members_v, sizeof(CHUNKS_HASH_T), _Alignof(CHUNKS_HASH_T),
												CHUNKS_HASH_NAME(member_drop));
		std_unwind_pop(&unwind);
		return result;
	}
	else
	{
		if (member_live)
			CHUNKS_HASH_NAME(member_drop) ((void *) &member);
		if (members_live)
			std_vec_drop_Checked((StdVec *) &members, sizeof(CHUNKS_HASH_T), _Alignof(CHUNKS_HASH_T),
								 CHUNKS_HASH_NAME(member_drop));
		if (iter_live)
			std_HashMap_IntoIter_drop((StdHashMapIntoIter *) &iter, CHUNKS_HASH_TYPE);
		std_unwind_resume(&unwind);
	}
	pg_unreachable();
}

/* The instance's &HashSet IntoIterator is iter. */
CHUNKS_HASH_DEF StdVecIntoIter
CHUNKS_HASH_REF_NAME(IntoIterator_into_iter) (const ChunksHashHashSet *self)
{
	return CHUNKS_HASH_NAME(iter) (self);
}

/* The instance's FromIterator is std's set's, of S's default hasher. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(FromIterator_from_iter) (StdIterator members, ChunksHashHashSet *result)
{
	CHUNKS_HASH_CAT(std_HashMap_from_iter_, CHUNKS_HASH_NAME(t)) (members, &result->f0);
}

/* The instance's Extend is std's set's. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(Extend_extend) (ChunksHashHashSet *self, StdIterator members)
{
	CHUNKS_HASH_CAT(std_HashMap_extend_, CHUNKS_HASH_NAME(t)) (&self->f0, members);
}

/* The instance's Debug writes the members in order as a set. */
CHUNKS_HASH_DEF bool
CHUNKS_HASH_NAME(Debug_fmt) (const void *value, StdFormatter *f)
{
	const ChunksHashHashSet *self = value;
	StdDebugList set = std_fmt_debug_set(f);
	volatile StdVecIntoIter members;
	volatile bool members_live = false;
	bool		result;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVecIntoIter *members_v = (StdVecIntoIter *) &members;
		const		CHUNKS_HASH_T *member;

		*members_v = CHUNKS_HASH_NAME(iter) (self);
		members_live = true;
		while (std_VecIntoIter_Iterator_next(members_v, &member))
			std_DebugSet_entry(&set, member, CHUNKS_HASH_T_DEBUG);
		members_live = false;
		std_VecIntoIter_drop(members_v);
		result = std_DebugSet_finish(&set);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (members_live)
			std_VecIntoIter_drop((StdVecIntoIter *) &members);
		std_unwind_resume(&unwind);
	}
	return result;
}

/* The instance's drop is the set's drop glue, std's map's. */
CHUNKS_HASH_DEF void
CHUNKS_HASH_NAME(drop) (ChunksHashHashSet *self)
{
	std_HashMap_drop(&self->f0, CHUNKS_HASH_TYPE);
}

#endif							/* CHUNKS_HASH_DEFINE */

#undef CHUNKS_HASH_NAME
#undef CHUNKS_HASH_REF_NAME

#endif							/* CHUNKS_HASH_SET */

#undef CHUNKS_HASH_CAT_
#undef CHUNKS_HASH_CAT
#undef CHUNKS_HASH_DEF_static
#undef CHUNKS_HASH_DEF_extern
#undef CHUNKS_HASH_DEF
#undef CHUNKS_HASH_RUST_FILE
#undef CHUNKS_HASH_SCOPE
#undef CHUNKS_HASH_DECLARE
#undef CHUNKS_HASH_DEFINE
#undef CHUNKS_HASH_TS
#undef CHUNKS_HASH_TYPE
#undef CHUNKS_HASH_ENTRY_FREEZE
#undef CHUNKS_HASH_ENTRY_COPY
#undef CHUNKS_HASH_ENTRY_BRANCHLESS
#undef CHUNKS_HASH_K
#undef CHUNKS_HASH_V
#undef CHUNKS_HASH_ENTRY
#undef CHUNKS_HASH_K_CMP
#undef CHUNKS_HASH_V_EQ
#undef CHUNKS_HASH_K_DEBUG
#undef CHUNKS_HASH_V_DEBUG
#undef CHUNKS_HASH_SET
#undef CHUNKS_HASH_T
#undef CHUNKS_HASH_T_LT
#undef CHUNKS_HASH_T_DEBUG
