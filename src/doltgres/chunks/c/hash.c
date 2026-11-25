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
 * The storage crates' hash map and set (chunks' hash.rs), whose instances
 * hash_template.h makes: this file holds the cases of the template's own
 * behavior, over test instances.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/hash.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/foldhash.h"
#include "doltgres/std/hash.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "hash.rs"

#ifdef DOLTGRES_TEST_CHUNKS

/* ChunksHashTestsCounted is the cases' key: an id, whose drops the cases count. */
typedef struct ChunksHashTestsCounted
{
	uint32		id;
} ChunksHashTestsCounted;

/* chunks_hash_tests_TupleCountedU32_t is the map case's entry, (Counted, u32). */
typedef struct chunks_hash_tests_TupleCountedU32_t
{
	ChunksHashTestsCounted f0;
	uint32		f1;
} chunks_hash_tests_TupleCountedU32_t;

/* chunks_hash_tests_DROPS counts the drops of the cases' keys. */
static uint64 chunks_hash_tests_DROPS;

/* chunks_hash_tests_Counted_drop is the key's drop glue, which counts. */
static void
chunks_hash_tests_Counted_drop(void *key)
{
	chunks_hash_tests_DROPS++;
}

/* chunks_hash_tests_Counted_Hash_hash is the key's Hash: its id's. */
static void
chunks_hash_tests_Counted_Hash_hash(const void *key, StdHasher state)
{
	std_Hasher_write_u32(state, ((const ChunksHashTestsCounted *) key)->id);
}

/* chunks_hash_tests_Counted_PartialEq_eq is the key's PartialEq: its ids'. */
static bool
chunks_hash_tests_Counted_PartialEq_eq(const void *a, const void *b)
{
	return ((const ChunksHashTestsCounted *) a)->id == ((const ChunksHashTestsCounted *) b)->id;
}

/* chunks_hash_tests_Counted_Ord_cmp is the key's Ord: its ids'. */
static StdOrdering
chunks_hash_tests_Counted_Ord_cmp(const ChunksHashTestsCounted *a, const ChunksHashTestsCounted *b)
{
	return a->id < b->id ? STD_ORDERING_LESS : (a->id > b->id ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* chunks_hash_tests_Counted_lt is the key's PartialOrd::lt as a sort's is_less. */
static bool
chunks_hash_tests_Counted_lt(void *captures, const void *a, const void *b)
{
	return ((const ChunksHashTestsCounted *) a)->id < ((const ChunksHashTestsCounted *) b)->id;
}

/* chunks_hash_tests_Counted_Debug_fmt is the key's Debug: its id's. */
static bool
chunks_hash_tests_Counted_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_u32_Debug_fmt(&((const ChunksHashTestsCounted *) value)->id, f);
}

/* chunks_hash_tests_u32_PartialEq_eq is u32's PartialEq over pointers. */
static bool
chunks_hash_tests_u32_PartialEq_eq(const void *a, const void *b)
{
	return *(const uint32 *) a == *(const uint32 *) b;
}

/* chunks_hash_tests_RandomState_Default_default is std's RandomState's Default. */
static void
chunks_hash_tests_RandomState_Default_default(void *result)
{
	*(StdRandomState *) result = std_RandomState_new();
}

/* CHUNKS_HASH_TESTS_MAP_TYPE is the table of a HashMap<Counted, u32>. */
static const StdHashMapType CHUNKS_HASH_TESTS_MAP_TYPE = {
	STD_HASHMAP_SHAPE(chunks_hash_tests_TupleCountedU32_t),
	.key_hash = chunks_hash_tests_Counted_Hash_hash,
	.key_eq = chunks_hash_tests_Counted_PartialEq_eq,
	.key_drop = chunks_hash_tests_Counted_drop,
	.hasher = &STD_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = chunks_hash_tests_RandomState_Default_default,
};

/* CHUNKS_HASH_TESTS_SET_TYPE is the table of a HashSet<Counted>. */
static const StdHashMapType CHUNKS_HASH_TESTS_SET_TYPE = {
	STD_HASHSET_SHAPE(ChunksHashTestsCounted),
	.key_hash = chunks_hash_tests_Counted_Hash_hash,
	.key_eq = chunks_hash_tests_Counted_PartialEq_eq,
	.key_drop = chunks_hash_tests_Counted_drop,
	.hasher = &STD_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = chunks_hash_tests_RandomState_Default_default,
};

/* The HashMap<Counted, u32> instance of the cases. */
#define CHUNKS_HASH_SCOPE extern
#define CHUNKS_HASH_DECLARE
#define CHUNKS_HASH_DEFINE
#define CHUNKS_HASH_TS CountedU32
#define CHUNKS_HASH_TYPE (&CHUNKS_HASH_TESTS_MAP_TYPE)
#define CHUNKS_HASH_ENTRY_FREEZE 1
#define CHUNKS_HASH_ENTRY_COPY 0
#define CHUNKS_HASH_ENTRY_BRANCHLESS 1
#define CHUNKS_HASH_K ChunksHashTestsCounted
#define CHUNKS_HASH_V uint32
#define CHUNKS_HASH_ENTRY chunks_hash_tests_TupleCountedU32_t
#define CHUNKS_HASH_K_CMP chunks_hash_tests_Counted_Ord_cmp
#define CHUNKS_HASH_V_EQ chunks_hash_tests_u32_PartialEq_eq
#define CHUNKS_HASH_K_DEBUG chunks_hash_tests_Counted_Debug_fmt
#define CHUNKS_HASH_V_DEBUG std_u32_Debug_fmt
#include "doltgres/chunks/hash_template.h"

/* The HashSet<Counted> instance of the cases. */
#define CHUNKS_HASH_SET
#define CHUNKS_HASH_SCOPE extern
#define CHUNKS_HASH_DECLARE
#define CHUNKS_HASH_DEFINE
#define CHUNKS_HASH_TS Counted
#define CHUNKS_HASH_TYPE (&CHUNKS_HASH_TESTS_SET_TYPE)
#define CHUNKS_HASH_ENTRY_FREEZE 1
#define CHUNKS_HASH_ENTRY_COPY 0
#define CHUNKS_HASH_ENTRY_BRANCHLESS 1
#define CHUNKS_HASH_T ChunksHashTestsCounted
#define CHUNKS_HASH_T_LT chunks_hash_tests_Counted_lt
#define CHUNKS_HASH_T_DEBUG chunks_hash_tests_Counted_Debug_fmt
#include "doltgres/chunks/hash_template.h"

/* chunks_hash_tests_oom is the cases' allocation error hook: a failed allocation panics, as pg's hook unwinds. */
static void
chunks_hash_tests_oom(size_t size, size_t align)
{
	std_panic(__FILE__, __LINE__, 0, "out of memory", sizeof("out of memory") - 1);
}

/* CHUNKS_HASH_TESTS_OOM_HOOK is the cases' hook, set while a case fails an allocation. */
static const StdAllocErrorHook CHUNKS_HASH_TESTS_OOM_HOOK = {chunks_hash_tests_oom};

/* ChunksHashTestsCase is a case's captures: whether it takes a set, and how many keys. */
typedef struct ChunksHashTestsCase
{
	bool		set;
	uint32		keys;
} ChunksHashTestsCase;

/*
 * chunks_hash_tests_into_iter is the cases' body: a map or a set of the
 * keys, whose into_iter's one allocation, its collected vector's, fails.
 */
static void
chunks_hash_tests_into_iter(void *captures)
{
	ChunksHashTestsCase *c = captures;
	StdVecIntoIter iter;

	if (c->set)
	{
		ChunksHashHashSet set;

		chunks_hash_HashSetCounted_Default_default(&set);
		for (uint32 i = 0; i < c->keys; i++)
		{
			ChunksHashTestsCounted key = {i};

			(void) chunks_hash_HashSetCounted_insert(&set, &key);
		}
		test_std_alloc_tests_fail_allocation(1);
		iter = chunks_hash_HashSetCounted_IntoIterator_into_iter(&set);
	}
	else
	{
		ChunksHashHashMap map;

		chunks_hash_HashMapCountedU32_Default_default(&map);
		for (uint32 i = 0; i < c->keys; i++)
		{
			ChunksHashTestsCounted key = {i};
			uint32		value = i;
			uint32		old;

			(void) chunks_hash_HashMapCountedU32_insert(&map, &key, &value, &old);
		}
		test_std_alloc_tests_fail_allocation(1);
		iter = chunks_hash_HashMapCountedU32_IntoIterator_into_iter(&map);
	}
	test_std_alloc_tests_fail_allocation(0);
	std_VecIntoIter_drop(&iter);
}

/* chunks_hash_tests_run runs a case and checks that it panicked and dropped every key once, as Rust's does. */
static void
chunks_hash_tests_run(bool set, uint32 keys)
{
	ChunksHashTestsCase c = {set, keys};
	StdCaught	caught;
	bool		ok;

	chunks_hash_tests_DROPS = 0;
	std_alloc_set_alloc_error_hook(&CHUNKS_HASH_TESTS_OOM_HOOK);
	ok = std_catch_unwind(chunks_hash_tests_into_iter, &c, &caught);
	test_std_alloc_tests_fail_allocation(0);
	std_alloc_set_alloc_error_hook(NULL);
	if (ok)
		std_panic(__FILE__, __LINE__, 0, "into_iter's allocation didn't fail",
				  sizeof("into_iter's allocation didn't fail") - 1);
	std_Caught_drop(&caught);
	if (chunks_hash_tests_DROPS != keys)
		std_panic(__FILE__, __LINE__, 0, "the keys weren't each dropped once",
				  sizeof("the keys weren't each dropped once") - 1);
}

/*
 * test_chunks_hash_tests_a_failed_into_iter_drops_every_entry takes maps of
 * 3 and of 20 entries into their sorted vectors with the vector's allocation
 * failing: every entry is dropped once, the one taken out of the table
 * included, as Rust 1.99.1's collect drops 3 of 3 and 20 of 20.
 */
void
test_chunks_hash_tests_a_failed_into_iter_drops_every_entry(void)
{
	chunks_hash_tests_run(false, 3);
	chunks_hash_tests_run(false, 20);
}

/*
 * test_chunks_hash_tests_a_failed_into_iter_drops_every_member is the set's
 * case of a failed into_iter: every member is dropped once.
 */
void
test_chunks_hash_tests_a_failed_into_iter_drops_every_member(void)
{
	chunks_hash_tests_run(true, 3);
	chunks_hash_tests_run(true, 20);
}

/* chunks_hash_tests_u32_Hash_hash is u32's Hash over a pointer. */
static void
chunks_hash_tests_u32_Hash_hash(const void *key, StdHasher state)
{
	std_Hasher_write_u32(state, *(const uint32 *) key);
}

/* chunks_hash_tests_u32_Ord_cmp is u32's Ord over pointers. */
static StdOrdering
chunks_hash_tests_u32_Ord_cmp(const uint32 *a, const uint32 *b)
{
	return *a < *b ? STD_ORDERING_LESS : (*a > *b ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* chunks_hash_tests_u32_lt is u32's PartialOrd::lt as a sort's is_less. */
static bool
chunks_hash_tests_u32_lt(void *captures, const void *a, const void *b)
{
	return *(const uint32 *) a < *(const uint32 *) b;
}

/* CHUNKS_HASH_TESTS_U32_MAP_TYPE is the table of a HashMap<u32, u32> of the walk tests. */
static const StdHashMapType CHUNKS_HASH_TESTS_U32_MAP_TYPE = {
	STD_HASHMAP_SHAPE(std_TupleU32U32_t),
	.key_hash = chunks_hash_tests_u32_Hash_hash,
	.key_eq = chunks_hash_tests_u32_PartialEq_eq,
	.hasher = &STD_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = chunks_hash_tests_RandomState_Default_default,
};

/* The HashMap<u32, u32> instance U32U32 of the walk tests. */
#define CHUNKS_HASH_SCOPE extern
#define CHUNKS_HASH_DECLARE
#define CHUNKS_HASH_DEFINE
#define CHUNKS_HASH_TS U32U32
#define CHUNKS_HASH_TYPE (&CHUNKS_HASH_TESTS_U32_MAP_TYPE)
#define CHUNKS_HASH_ENTRY_FREEZE 1
#define CHUNKS_HASH_ENTRY_COPY 1
#define CHUNKS_HASH_ENTRY_BRANCHLESS 1
#define CHUNKS_HASH_K uint32
#define CHUNKS_HASH_V uint32
#define CHUNKS_HASH_ENTRY std_TupleU32U32_t
#define CHUNKS_HASH_K_CMP chunks_hash_tests_u32_Ord_cmp
#define CHUNKS_HASH_V_EQ chunks_hash_tests_u32_PartialEq_eq
#define CHUNKS_HASH_K_DEBUG std_u32_Debug_fmt
#define CHUNKS_HASH_V_DEBUG std_u32_Debug_fmt
#include "doltgres/chunks/hash_template.h"

/* CHUNKS_HASH_TESTS_U32_FAST_MAP_TYPE is the table of a HashMap<u32, u32> of the walk tests. */
static const StdHashMapType CHUNKS_HASH_TESTS_U32_FAST_MAP_TYPE = {
	STD_HASHMAP_SHAPE(std_TupleU32U32_t),
	.key_hash = chunks_hash_tests_u32_Hash_hash,
	.key_eq = chunks_hash_tests_u32_PartialEq_eq,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/* The HashMap<u32, u32> instance U32U32Fast of the walk tests. */
#define CHUNKS_HASH_SCOPE extern
#define CHUNKS_HASH_DECLARE
#define CHUNKS_HASH_DEFINE
#define CHUNKS_HASH_TS U32U32Fast
#define CHUNKS_HASH_TYPE (&CHUNKS_HASH_TESTS_U32_FAST_MAP_TYPE)
#define CHUNKS_HASH_ENTRY_FREEZE 1
#define CHUNKS_HASH_ENTRY_COPY 1
#define CHUNKS_HASH_ENTRY_BRANCHLESS 1
#define CHUNKS_HASH_K uint32
#define CHUNKS_HASH_V uint32
#define CHUNKS_HASH_ENTRY std_TupleU32U32_t
#define CHUNKS_HASH_K_CMP chunks_hash_tests_u32_Ord_cmp
#define CHUNKS_HASH_V_EQ chunks_hash_tests_u32_PartialEq_eq
#define CHUNKS_HASH_K_DEBUG std_u32_Debug_fmt
#define CHUNKS_HASH_V_DEBUG std_u32_Debug_fmt
#include "doltgres/chunks/hash_template.h"

/* CHUNKS_HASH_TESTS_U32_SET_TYPE is the table of a HashSet<u32> of the walk tests. */
static const StdHashMapType CHUNKS_HASH_TESTS_U32_SET_TYPE = {
	STD_HASHSET_SHAPE(uint32),
	.key_hash = chunks_hash_tests_u32_Hash_hash,
	.key_eq = chunks_hash_tests_u32_PartialEq_eq,
	.hasher = &STD_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = chunks_hash_tests_RandomState_Default_default,
};

/* The HashSet<u32> instance U32 of the walk tests. */
#define CHUNKS_HASH_SET
#define CHUNKS_HASH_SCOPE extern
#define CHUNKS_HASH_DECLARE
#define CHUNKS_HASH_DEFINE
#define CHUNKS_HASH_TS U32
#define CHUNKS_HASH_TYPE (&CHUNKS_HASH_TESTS_U32_SET_TYPE)
#define CHUNKS_HASH_ENTRY_FREEZE 1
#define CHUNKS_HASH_ENTRY_COPY 1
#define CHUNKS_HASH_ENTRY_BRANCHLESS 1
#define CHUNKS_HASH_T uint32
#define CHUNKS_HASH_T_LT chunks_hash_tests_u32_lt
#define CHUNKS_HASH_T_DEBUG std_u32_Debug_fmt
#include "doltgres/chunks/hash_template.h"

/* CHUNKS_HASH_TESTS_U32_FAST_SET_TYPE is the table of a HashSet<u32> of the walk tests. */
static const StdHashMapType CHUNKS_HASH_TESTS_U32_FAST_SET_TYPE = {
	STD_HASHSET_SHAPE(uint32),
	.key_hash = chunks_hash_tests_u32_Hash_hash,
	.key_eq = chunks_hash_tests_u32_PartialEq_eq,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/* The HashSet<u32> instance U32Fast of the walk tests. */
#define CHUNKS_HASH_SET
#define CHUNKS_HASH_SCOPE extern
#define CHUNKS_HASH_DECLARE
#define CHUNKS_HASH_DEFINE
#define CHUNKS_HASH_TS U32Fast
#define CHUNKS_HASH_TYPE (&CHUNKS_HASH_TESTS_U32_FAST_SET_TYPE)
#define CHUNKS_HASH_ENTRY_FREEZE 1
#define CHUNKS_HASH_ENTRY_COPY 1
#define CHUNKS_HASH_ENTRY_BRANCHLESS 1
#define CHUNKS_HASH_T uint32
#define CHUNKS_HASH_T_LT chunks_hash_tests_u32_lt
#define CHUNKS_HASH_T_DEBUG std_u32_Debug_fmt
#include "doltgres/chunks/hash_template.h"

/* chunks_hash_tests_u32s_Debug_fmt is Vec<u32>'s Debug. */
static bool
chunks_hash_tests_u32s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&list, &STD_VEC_AT(v, uint32, i), std_u32_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* chunks_hash_tests_walked checks that the values walked are 0 to 99 in order, as assert_eq! with sorted does. */
static void
chunks_hash_tests_walked(StdVec *walked, uint32 line)
{
	StdVec		sorted = STD_VEC_NEW;
	bool		same = walked->len == 100;

	for (uint32 i = 0; i < 100; i++)
		std_vec_push_Checked(&sorted, &i, sizeof(uint32), _Alignof(uint32));
	for (size_t i = 0; same && i < 100; i++)
		same = STD_VEC_AT(walked, uint32, i) == (uint32) i;
	if (!same)
		std_assert_failed(RUST_FILE, line, 9, "==", walked, chunks_hash_tests_u32s_Debug_fmt, &sorted,
						  chunks_hash_tests_u32s_Debug_fmt, NULL);
	std_vec_free_Checked(&sorted, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(walked, sizeof(uint32), _Alignof(uint32));
}

/* chunks_hash_tests_pairs_keys collects the keys of an iter's pairs. */
static StdVec
chunks_hash_tests_pairs_keys(StdVecIntoIter iter)
{
	StdVec		keys = STD_VEC_NEW;
	chunks_hash_HashMapU32U32_iter_t pair;

	while (std_VecIntoIter_Iterator_next(&iter, &pair))
		std_vec_push_Checked(&keys, pair.f0, sizeof(uint32), _Alignof(uint32));
	std_VecIntoIter_drop(&iter);
	return keys;
}

/* chunks_hash_tests_refs collects the members of an iter of references. */
static StdVec
chunks_hash_tests_refs(StdVecIntoIter iter)
{
	StdVec		members = STD_VEC_NEW;
	const uint32 *member;

	while (std_VecIntoIter_Iterator_next(&iter, &member))
		std_vec_push_Checked(&members, member, sizeof(uint32), _Alignof(uint32));
	std_VecIntoIter_drop(&iter);
	return members;
}

/* chunks_hash_tests_printed checks that a Debug text starts and ends as the test's assert! asks. */
static void
chunks_hash_tests_printed(const StdString *printed, const char *start, const char *end, uint32 line)
{
	size_t		s = strlen(start);
	size_t		e = strlen(end);

	if (!(printed->len >= s && memcmp(printed->ptr, start, s) == 0 && printed->len >= e &&
		  memcmp((const char *) printed->ptr + printed->len - e, end, e) == 0))
		std_panic(RUST_FILE, line, 9, printed->ptr, printed->len);
}

/*
 * test_chunks_hash_tests_a_map_is_walked_in_key_order checks that a map is
 * walked in key order by its entries, keys, values, and owned entries, and
 * printed in it, whatever order its keys went in.
 */
void
test_chunks_hash_tests_a_map_is_walked_in_key_order(void)
{
	ChunksHashHashMap map;
	ChunksHashHashMap fast;
	StdVec		walked = STD_VEC_NEW;
	chunks_hash_HashMapU32U32_keys_iter keys;
	chunks_hash_HashMapU32U32Fast_keys_iter fast_keys;
	chunks_hash_HashMapU32U32_values_iter values;
	const uint32 *key;
	const uint32 *value;
	StdVecIntoIter owned;
	std_TupleU32U32_t entry;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&map, chunks_hash_HashMapU32U32_Debug_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};
	StdString	printed;

	chunks_hash_HashMapU32U32_Default_default(&map);
	chunks_hash_HashMapU32U32Fast_Default_default(&fast);
	for (uint32 i = 0; i < 100; i++)
	{
		uint32		k = (i * 37) % 100;
		uint32		v = k * 2;
		uint32		w = k;
		uint32		old;

		(void) chunks_hash_HashMapU32U32_insert(&map, &k, &v, &old);
		(void) chunks_hash_HashMapU32U32Fast_insert(&fast, &k, &w, &old);
	}
	walked = chunks_hash_tests_pairs_keys(chunks_hash_HashMapU32U32_iter(&map));
	chunks_hash_tests_walked(&walked, 323);
	walked = chunks_hash_tests_pairs_keys(chunks_hash_RefHashMapU32U32_IntoIterator_into_iter(&map));
	chunks_hash_tests_walked(&walked, 324);
	walked = (StdVec) STD_VEC_NEW;
	keys = chunks_hash_HashMapU32U32_keys(&map);
	while (chunks_hash_HashMapU32U32_keys_iter_next(&keys, &key))
		std_vec_push_Checked(&walked, key, sizeof(uint32), _Alignof(uint32));
	std_VecIntoIter_drop(&keys.inner);
	chunks_hash_tests_walked(&walked, 325);
	walked = (StdVec) STD_VEC_NEW;
	values = chunks_hash_HashMapU32U32_values(&map);
	while (chunks_hash_HashMapU32U32_values_iter_next(&values, &value))
	{
		uint32		half = *value / 2;

		std_vec_push_Checked(&walked, &half, sizeof(uint32), _Alignof(uint32));
	}
	std_VecIntoIter_drop(&values.inner);
	chunks_hash_tests_walked(&walked, 326);
	std_fmt_format_Checked(&printed, &arguments);
	chunks_hash_tests_printed(&printed, "{0: 0, 1: 2, 2: 4, ", ", 99: 198}", 328);
	std_vec_free_Checked(&printed, 1, 1);
	walked = (StdVec) STD_VEC_NEW;
	owned = chunks_hash_HashMapU32U32_IntoIterator_into_iter(&map);
	while (std_VecIntoIter_Iterator_next(&owned, &entry))
		std_vec_push_Checked(&walked, &entry.f0, sizeof(uint32), _Alignof(uint32));
	std_VecIntoIter_drop(&owned);
	chunks_hash_tests_walked(&walked, 329);
	walked = (StdVec) STD_VEC_NEW;
	fast_keys = chunks_hash_HashMapU32U32Fast_keys(&fast);
	while (chunks_hash_HashMapU32U32Fast_keys_iter_next(&fast_keys, &key))
		std_vec_push_Checked(&walked, key, sizeof(uint32), _Alignof(uint32));
	std_VecIntoIter_drop(&fast_keys.inner);
	chunks_hash_tests_walked(&walked, 331);
	chunks_hash_HashMapU32U32Fast_drop(&fast);
}

/*
 * test_chunks_hash_tests_a_set_is_walked_in_order checks that a set is
 * walked in order by its members and owned members, and printed in it.
 */
void
test_chunks_hash_tests_a_set_is_walked_in_order(void)
{
	ChunksHashHashSet set;
	ChunksHashHashSet fast;
	StdVec		walked;
	StdVecIntoIter owned;
	uint32		member;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&set, chunks_hash_HashSetU32_Debug_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};
	StdString	printed;

	chunks_hash_HashSetU32_Default_default(&set);
	chunks_hash_HashSetU32Fast_Default_default(&fast);
	for (uint32 i = 0; i < 100; i++)
	{
		uint32		k = (i * 37) % 100;
		uint32		l = k;

		(void) chunks_hash_HashSetU32_insert(&set, &k);
		(void) chunks_hash_HashSetU32Fast_insert(&fast, &l);
	}
	walked = chunks_hash_tests_refs(chunks_hash_HashSetU32_iter(&set));
	chunks_hash_tests_walked(&walked, 339);
	walked = chunks_hash_tests_refs(chunks_hash_RefHashSetU32_IntoIterator_into_iter(&set));
	chunks_hash_tests_walked(&walked, 340);
	std_fmt_format_Checked(&printed, &arguments);
	chunks_hash_tests_printed(&printed, "{0, 1, 2, ", ", 99}", 342);
	std_vec_free_Checked(&printed, 1, 1);
	walked = (StdVec) STD_VEC_NEW;
	owned = chunks_hash_HashSetU32_IntoIterator_into_iter(&set);
	while (std_VecIntoIter_Iterator_next(&owned, &member))
		std_vec_push_Checked(&walked, &member, sizeof(uint32), _Alignof(uint32));
	std_VecIntoIter_drop(&owned);
	chunks_hash_tests_walked(&walked, 343);
	walked = chunks_hash_tests_refs(chunks_hash_HashSetU32Fast_iter(&fast));
	chunks_hash_tests_walked(&walked, 345);
	chunks_hash_HashSetU32Fast_drop(&fast);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
