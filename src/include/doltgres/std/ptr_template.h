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
 * The template of core's ptr generics that hold their T by value (the port
 * text's section 2): ptr::swap<T>, ported from core/src/ptr/mod.rs at Rust
 * 1.99.1, whose copies run their precondition checks, as a crate's
 * codegen of it does. STD_PTR_SCOPE is static or extern, STD_PTR_DECLARE
 * and STD_PTR_DEFINE choose the declarations and definitions, STD_PTR_TS
 * spells T, and STD_PTR_T is T; every parameter is undefined at the end.
 */

/* STD_PTR_CAT pastes two tokens after expanding them. */
#define STD_PTR_CAT_(a, b) a##b
#define STD_PTR_CAT(a, b) STD_PTR_CAT_(a, b)

/* STD_PTR_DEF is STD_PTR_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_PTR_DEF_static static
#define STD_PTR_DEF_extern
#define STD_PTR_DEF STD_PTR_CAT(STD_PTR_DEF_, STD_PTR_SCOPE)

/* STD_PTR_SWAP is ptr::swap's instance at T. */
#define STD_PTR_SWAP STD_PTR_CAT(std_ptr_swap_, STD_PTR_TS)

#ifdef STD_PTR_DECLARE
STD_PTR_SCOPE void STD_PTR_SWAP(STD_PTR_T * x, STD_PTR_T * y);
#endif

#ifdef STD_PTR_DEFINE

/*
 * ptr::swap swaps the values at two places of T, which may overlap, through
 * a temporary of T: a copy of x out, y copied onto x, and the copy onto y.
 */
STD_PTR_DEF void
STD_PTR_SWAP(STD_PTR_T * x, STD_PTR_T * y)
{
	STD_PTR_T	tmp;

	std_ptr_copy_nonoverlapping_Checked(x, &tmp, 1, sizeof(STD_PTR_T), _Alignof(STD_PTR_T));
	std_ptr_copy(y, x, 1, sizeof(STD_PTR_T), _Alignof(STD_PTR_T));
	std_ptr_copy_nonoverlapping_Checked(&tmp, y, 1, sizeof(STD_PTR_T), _Alignof(STD_PTR_T));
}

#endif							/* STD_PTR_DEFINE */

#undef STD_PTR_CAT_
#undef STD_PTR_CAT
#undef STD_PTR_DEF_static
#undef STD_PTR_DEF_extern
#undef STD_PTR_DEF
#undef STD_PTR_SWAP
#undef STD_PTR_SCOPE
#undef STD_PTR_DECLARE
#undef STD_PTR_DEFINE
#undef STD_PTR_TS
#undef STD_PTR_T
