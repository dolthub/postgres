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
 * The template of core's slice search generic that holds its key by value
 * (the port text's section 2): binary_search_by_key at a key type B, whose
 * closure holds f's key. An instance defines its parameters and includes
 * this file; every parameter is undefined at the end.
 *
 * STD_SLICE_SEARCH_SCOPE is static for an instance one file alone uses, and
 * extern for one defined in one file and declared in others.
 * STD_SLICE_SEARCH_DECLARE generates the instance's declarations, and
 * STD_SLICE_SEARCH_DEFINE its definitions, which its declarations precede.
 * STD_SLICE_SEARCH_BS spells B, STD_SLICE_SEARCH_B is B, and
 * STD_SLICE_SEARCH_B_CMP is B's Ord::cmp over two const B pointers.
 */

/* STD_SLICE_SEARCH_CAT pastes two tokens after expanding them. */
#define STD_SLICE_SEARCH_CAT_(a, b) a##b
#define STD_SLICE_SEARCH_CAT(a, b) STD_SLICE_SEARCH_CAT_(a, b)

/* STD_SLICE_SEARCH_DEF is STD_SLICE_SEARCH_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_SLICE_SEARCH_DEF_static static
#define STD_SLICE_SEARCH_DEF_extern
#define STD_SLICE_SEARCH_DEF STD_SLICE_SEARCH_CAT(STD_SLICE_SEARCH_DEF_, STD_SLICE_SEARCH_SCOPE)

/* STD_SLICE_SEARCH_NAME is a generic's name at the instance's B. */
#define STD_SLICE_SEARCH_NAME(name) STD_SLICE_SEARCH_CAT(name, STD_SLICE_SEARCH_BS)

/* STD_SLICE_SEARCH_CLOSURE is the instance's closure's captures' type. */
#define STD_SLICE_SEARCH_CLOSURE STD_SLICE_SEARCH_NAME(std_slice_binary_search_by_key_closure_0_t)

#ifdef STD_SLICE_SEARCH_DECLARE

/* The instance's closure's captures: the borrowed f and b. */
typedef struct STD_SLICE_SEARCH_CLOSURE
{
	STD_SLICE_SEARCH_B(*f) (void *captures, const void *elem);
	void	   *f_captures;
	const		STD_SLICE_SEARCH_B *b;
}			STD_SLICE_SEARCH_CLOSURE;

STD_SLICE_SEARCH_SCOPE bool STD_SLICE_SEARCH_NAME(std_slice_binary_search_by_key) (const void *self, size_t len, size_t elem_size,
																				   const STD_SLICE_SEARCH_B * b,
																				   STD_SLICE_SEARCH_B(*f) (void *captures, const void *elem),
																				   void *f_captures,
																				   void (*f_drop) (void *),
																				   size_t *result);

#endif							/* STD_SLICE_SEARCH_DECLARE */

#ifdef STD_SLICE_SEARCH_DEFINE

/* std_slice_binary_search_by_key_closure_0 is binary_search_by_key's |k| f(k).cmp(b). */
static StdOrdering
STD_SLICE_SEARCH_NAME(std_slice_binary_search_by_key_closure_0) (void *captures, const void *k)
{
	STD_SLICE_SEARCH_CLOSURE *c = (STD_SLICE_SEARCH_CLOSURE *) captures;
	STD_SLICE_SEARCH_B key = c->f(c->f_captures, k);

	return STD_SLICE_SEARCH_B_CMP(&key, c->b);
}

/*
 * std_slice_binary_search_by_key is <[T]>::binary_search_by_key: f's key of
 * the elements it probes against b, true with the index of one whose key is
 * b in result, or false with where one would go.
 */
STD_SLICE_SEARCH_DEF bool
STD_SLICE_SEARCH_NAME(std_slice_binary_search_by_key) (const void *self, size_t len, size_t elem_size,
													   const STD_SLICE_SEARCH_B * b,
													   STD_SLICE_SEARCH_B(*f) (void *captures, const void *elem),
													   void *f_captures, void (*f_drop) (void *), size_t *result)
{
	STD_SLICE_SEARCH_CLOSURE captures = {f, f_captures, b};
	bool		ok = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		ok = std_slice_binary_search_by(self, len, elem_size, STD_SLICE_SEARCH_NAME(std_slice_binary_search_by_key_closure_0),
										&captures, NULL, result);
		if (f_drop != NULL)
			f_drop(f_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (f_drop != NULL)
			f_drop(f_captures);
		std_unwind_resume(&unwind);
	}
	return ok;
}

#endif							/* STD_SLICE_SEARCH_DEFINE */

#undef STD_SLICE_SEARCH_CAT_
#undef STD_SLICE_SEARCH_CAT
#undef STD_SLICE_SEARCH_DEF_static
#undef STD_SLICE_SEARCH_DEF_extern
#undef STD_SLICE_SEARCH_DEF
#undef STD_SLICE_SEARCH_NAME
#undef STD_SLICE_SEARCH_CLOSURE
#undef STD_SLICE_SEARCH_SCOPE
#undef STD_SLICE_SEARCH_DECLARE
#undef STD_SLICE_SEARCH_DEFINE
#undef STD_SLICE_SEARCH_BS
#undef STD_SLICE_SEARCH_B
#undef STD_SLICE_SEARCH_B_CMP
