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
 * The template of core's slice methods that reach a generic holding T by
 * value (the port text's section 2): swap and swap_unchecked, ported from
 * core/src/slice/mod.rs at Rust 1.99.1, which reach ptr::swap<T> through
 * ptr_template.h's instance at the same T, declared before this inclusion.
 * Both are #[track_caller], so their panics and checks are at their
 * caller's location. STD_SLICE_SCOPE is static or extern, STD_SLICE_DECLARE
 * and STD_SLICE_DEFINE choose the declarations and definitions,
 * STD_SLICE_TS spells T, and STD_SLICE_T is T; every parameter is undefined
 * at the end.
 */

/* STD_SLICE_CAT pastes two tokens after expanding them. */
#define STD_SLICE_CAT_(a, b) a##b
#define STD_SLICE_CAT(a, b) STD_SLICE_CAT_(a, b)

/* STD_SLICE_DEF is STD_SLICE_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_SLICE_DEF_static static
#define STD_SLICE_DEF_extern
#define STD_SLICE_DEF STD_SLICE_CAT(STD_SLICE_DEF_, STD_SLICE_SCOPE)

/* STD_SLICE_NAME names the methods' instances at T, and STD_SLICE_PTR_SWAP is ptr::swap's. */
#define STD_SLICE_NAME(name) STD_SLICE_CAT(std_slice_##name##_, STD_SLICE_TS)
#define STD_SLICE_PTR_SWAP STD_SLICE_CAT(std_ptr_swap_, STD_SLICE_TS)

#ifdef STD_SLICE_DECLARE
STD_SLICE_SCOPE void STD_SLICE_NAME(swap) (STD_SLICE_T * self, size_t len, size_t a, size_t b, const char *file,
										   uint32 line, uint32 col);
STD_SLICE_SCOPE void STD_SLICE_NAME(swap_unchecked) (STD_SLICE_T * self, size_t len, size_t a, size_t b,
													 const char *file, uint32 line, uint32 col);
#endif

#ifdef STD_SLICE_DEFINE

/* swap swaps two elements, each index checked as indexing checks it. */
STD_SLICE_DEF void
STD_SLICE_NAME(swap) (STD_SLICE_T * self, size_t len, size_t a, size_t b, const char *file, uint32 line, uint32 col)
{
	std_index_check(a, len, file, line, col);
	std_index_check(b, len, file, line, col);
	STD_SLICE_NAME(swap_unchecked) (self, len, a, b, file, line, col);
}

/* swap_unchecked swaps two elements whose indices the caller promises are within the slice. */
STD_SLICE_DEF void
STD_SLICE_NAME(swap_unchecked) (STD_SLICE_T * self, size_t len, size_t a, size_t b, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!(a < len && b < len))
		STD_PTR_VIOLATED("slice::swap_unchecked requires that the indices are within the slice", file, line, col);
#endif
	STD_SLICE_PTR_SWAP(self + a, self + b);
}

#endif							/* STD_SLICE_DEFINE */

#undef STD_SLICE_CAT_
#undef STD_SLICE_CAT
#undef STD_SLICE_DEF_static
#undef STD_SLICE_DEF_extern
#undef STD_SLICE_DEF
#undef STD_SLICE_NAME
#undef STD_SLICE_PTR_SWAP
#undef STD_SLICE_SCOPE
#undef STD_SLICE_DECLARE
#undef STD_SLICE_DEFINE
#undef STD_SLICE_TS
#undef STD_SLICE_T
