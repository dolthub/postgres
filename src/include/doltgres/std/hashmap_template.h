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
 * The template of HashMap's generics that hold an entry by value (the port
 * text's sections 2 and 9): Extend<(K, V)>, hashbrown's, and FromIterator,
 * std's, at an entry type, each item a local of the entry's layout. An
 * instance defines its parameters and includes this file; every parameter is
 * undefined at the end.
 *
 * STD_HASHMAP_SCOPE is static or extern, STD_HASHMAP_DECLARE and
 * STD_HASHMAP_DEFINE choose the declarations and definitions,
 * STD_HASHMAP_TS spells the entry's types, STD_HASHMAP_ITEM is the (K, V)
 * tuple whose layout is the map's element's, STD_HASHMAP_V is V, or
 * STD_HASHMAP_UNIT is defined for a set's (), and STD_HASHMAP_TYPE is the
 * map's StdHashMapType.
 */

#include "doltgres/std/hashmap.h"
#include "doltgres/std/iter.h"

/* STD_HASHMAP_CAT pastes two tokens after expanding them. */
#define STD_HASHMAP_CAT_(a, b) a##b
#define STD_HASHMAP_CAT(a, b) STD_HASHMAP_CAT_(a, b)

/* STD_HASHMAP_DEF is STD_HASHMAP_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_HASHMAP_DEF_static static
#define STD_HASHMAP_DEF_extern
#define STD_HASHMAP_DEF STD_HASHMAP_CAT(STD_HASHMAP_DEF_, STD_HASHMAP_SCOPE)

/* STD_HASHMAP_NAME names the instance's functions. */
#define STD_HASHMAP_NAME(name) STD_HASHMAP_CAT(std_HashMap_##name##_, STD_HASHMAP_TS)

#ifdef STD_HASHMAP_DECLARE
STD_HASHMAP_SCOPE void STD_HASHMAP_NAME(extend) (StdHashMap *map, StdIterator iter);
STD_HASHMAP_SCOPE void STD_HASHMAP_NAME(from_iter) (StdIterator iter, StdHashMap *result);
#endif

#ifdef STD_HASHMAP_DEFINE

/*
 * The instance's extend is HashMap's Extend<(K, V)>, hashbrown's: room for
 * the iterator's lower bound when the map is empty and for half of one more
 * than it otherwise, then each item inserted, the value it replaces dropped.
 * It takes the iterator and drops it at the end, or should an insert or a
 * drop panic.
 */
STD_HASHMAP_DEF void
STD_HASHMAP_NAME(extend) (StdHashMap *map, StdIterator iter)
{
	const StdHashMapType *t = STD_HASHMAP_TYPE;
	STD_HASHMAP_ITEM item;
#ifndef STD_HASHMAP_UNIT
	STD_HASHMAP_V replaced;
#endif
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdSizeHint hint = iter.ops->size_hint(iter.self);

		std_HashMap_reserve(map, t, std_HashMap_is_empty(map) ? hint.lower : (hint.lower + 1) / 2);
		while (iter.ops->next(iter.self, &item))
		{
#ifndef STD_HASHMAP_UNIT
			if (std_HashMap_insert(map, t, &item, (char *) &item + t->value_offset, &replaced) &&
				t->value_drop != NULL)
				t->value_drop(&replaced);
#else
			(void) std_HashMap_insert(map, t, &item, STD_DANGLING, STD_DANGLING);
#endif
		}
		std_iterator_drop(&iter);
		std_unwind_pop(&unwind);
	}
	else
	{
		std_iterator_drop(&iter);
		std_unwind_resume(&unwind);
	}
}

/*
 * The instance's from_iter is HashMap's FromIterator, std's: a map of S's
 * default hasher, extended by the iterator.
 */
STD_HASHMAP_DEF void
STD_HASHMAP_NAME(from_iter) (StdIterator iter, StdHashMap *result)
{
	volatile bool iter_live = true;
	volatile bool map_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_HashMap_Default_default(result, STD_HASHMAP_TYPE);
		map_live = true;
		iter_live = false;
		STD_HASHMAP_NAME(extend) (result, iter);
		map_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (map_live)
			std_HashMap_drop(result, STD_HASHMAP_TYPE);
		if (iter_live)
			std_iterator_drop(&iter);
		std_unwind_resume(&unwind);
	}
}

#endif							/* STD_HASHMAP_DEFINE */

#undef STD_HASHMAP_CAT_
#undef STD_HASHMAP_CAT
#undef STD_HASHMAP_DEF_static
#undef STD_HASHMAP_DEF_extern
#undef STD_HASHMAP_DEF
#undef STD_HASHMAP_NAME
#undef STD_HASHMAP_SCOPE
#undef STD_HASHMAP_DECLARE
#undef STD_HASHMAP_DEFINE
#undef STD_HASHMAP_TS
#undef STD_HASHMAP_ITEM
#undef STD_HASHMAP_V
#undef STD_HASHMAP_UNIT
#undef STD_HASHMAP_TYPE
