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
 * The template of core's #[inline] <[T]>::split_at, whose precondition
 * checks run where it is codegened (the port text's section 3): split_at,
 * split_at_checked, and split_at_unchecked, ported from
 * core/src/slice/mod.rs at Rust 1.99.1. A crate's split is codegened in the
 * crate, checked in the cassert build; core's own non-inline code's is not.
 * STD_SPLIT_DECLARE and STD_SPLIT_DEFINE choose the declarations and
 * definitions, STD_SPLIT_UB_CHECKS is 1 for the checked instance and 0 for
 * the other, and STD_SPLIT_SUFFIX names the instance; every parameter is
 * undefined at the end.
 */

/* STD_SPLIT_CAT pastes two tokens after expanding them. */
#define STD_SPLIT_CAT_(a, b) a##b
#define STD_SPLIT_CAT(a, b) STD_SPLIT_CAT_(a, b)

/* STD_SPLIT_NAME names the instance's functions. */
#define STD_SPLIT_NAME(name) STD_SPLIT_CAT(std_slice_##name##_, STD_SPLIT_SUFFIX)

#ifdef STD_SPLIT_DECLARE
extern void STD_SPLIT_NAME(split_at_unchecked) (const void *ptr, size_t len, size_t mid, size_t size,
												size_t align, const void **left, size_t *left_len,
												const void **right, size_t *right_len, const char *file,
												uint32 line, uint32 col);
extern void STD_SPLIT_NAME(split_at) (const void *ptr, size_t len, size_t mid, size_t size, size_t align,
									  const void **left, size_t *left_len, const void **right,
									  size_t *right_len, const char *file, uint32 line, uint32 col);
#endif

#ifdef STD_SPLIT_DEFINE

/*
 * split_at_unchecked is <[T]>::split_at_unchecked: the slice's two halves at
 * mid, its precondition and from_raw_parts' checked at its caller.
 */
void
STD_SPLIT_NAME(split_at_unchecked) (const void *ptr, size_t len, size_t mid, size_t size, size_t align,
									const void **left, size_t *left_len, const void **right,
									size_t *right_len, const char *file, uint32 line, uint32 col)
{
#if STD_SPLIT_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	if (!(mid <= len))
		STD_PTR_VIOLATED("slice::split_at_unchecked requires the index to be within the slice", file, line, col);
	std_slice_from_raw_parts_check(ptr, mid, size, align, file, line, col);
#endif
	*left = ptr;
	*left_len = mid;
#if STD_SPLIT_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_slice_from_raw_parts_check((const char *) ptr + mid * size, len - mid, size, align, file, line, col);
#endif
	*right = (const char *) ptr + mid * size;
	*right_len = len - mid;
}

/*
 * split_at_checked is <[T]>::split_at_checked: the slice's two halves at
 * mid, or false for a mid past the length.
 */
static bool
STD_SPLIT_NAME(split_at_checked) (const void *ptr, size_t len, size_t mid, size_t size, size_t align,
								  const void **left, size_t *left_len, const void **right, size_t *right_len)
{
	if (mid <= len)
	{
		STD_SPLIT_NAME(split_at_unchecked) (ptr, len, mid, size, align, left, left_len, right, right_len,
											STD_SLICE_MOD_FILE, 2166, 32);
		return true;
	}
	return false;
}

/*
 * split_at is <[T]>::split_at: the slice's two halves at mid, panicking with
 * "mid > len" at its caller for a mid past the length.
 */
void
STD_SPLIT_NAME(split_at) (const void *ptr, size_t len, size_t mid, size_t size, size_t align, const void **left,
						  size_t *left_len, const void **right, size_t *right_len, const char *file, uint32 line,
						  uint32 col)
{
	if (!STD_SPLIT_NAME(split_at_checked) (ptr, len, mid, size, align, left, left_len, right, right_len))
	{
		static const char text[] = "mid > len";

		std_panic(file, line, col, text, sizeof(text) - 1);
	}
}

#endif							/* STD_SPLIT_DEFINE */

#undef STD_SPLIT_CAT_
#undef STD_SPLIT_CAT
#undef STD_SPLIT_NAME
#undef STD_SPLIT_DECLARE
#undef STD_SPLIT_DEFINE
#undef STD_SPLIT_UB_CHECKS
#undef STD_SPLIT_SUFFIX
