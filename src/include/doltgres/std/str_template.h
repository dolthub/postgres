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
 * The template of core's #[inline] str iteration, whose precondition checks
 * run where it is codegened (the port text's section 3): Chars::next and
 * next_code_point, ported from core/src/str/iter.rs and
 * core/src/str/validations.rs at Rust 1.99.1. A crate's iteration is
 * codegened in the crate, checked in the cassert build; core's own non-inline
 * code's is not. STD_STR_SCOPE is static or extern, STD_STR_DECLARE and
 * STD_STR_DEFINE choose the declarations and definitions, STD_STR_UB_CHECKS
 * is 1 for the checked instance and 0 for the other, and STD_STR_SUFFIX
 * names the instance; every parameter is undefined at the end.
 */

/* STD_STR_CAT pastes two tokens after expanding them. */
#define STD_STR_CAT_(a, b) a##b
#define STD_STR_CAT(a, b) STD_STR_CAT_(a, b)

/* STD_STR_DEF is STD_STR_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_STR_DEF_static static
#define STD_STR_DEF_extern
#define STD_STR_DEF STD_STR_CAT(STD_STR_DEF_, STD_STR_SCOPE)

/* STD_STR_NAME names the instance's functions. */
#define STD_STR_NAME(name) STD_STR_CAT(std_str_##name##_, STD_STR_SUFFIX)

#ifdef STD_STR_DECLARE
STD_STR_SCOPE bool STD_STR_NAME(next_code_point) (StdSliceIterU8 *bytes, uint32 *result);
STD_STR_SCOPE bool STD_STR_NAME(Chars_next) (StdChars *self, uint32 *result);
#endif

#ifdef STD_STR_DEFINE

/*
 * next_code_point is the next code point of an iterator over valid UTF-8's
 * bytes: one byte below 128, or a lead byte and its continuation bytes,
 * which the UTF-8 promises are there, so a missing one is unreachable.
 */
STD_STR_DEF bool
STD_STR_NAME(next_code_point) (StdSliceIterU8 *bytes, uint32 *result)
{
	uint8		x;
	uint32		init;
	uint8		y;
	uint32		ch;

	if (bytes->ptr == bytes->end)
		return false;
	x = *bytes->ptr++;
	if (x < 128)
	{
		*result = x;
		return true;
	}
	init = std_str_utf8_first_byte(x, 2);
	if (bytes->ptr == bytes->end)
	{
#if STD_STR_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_hint_unreachable_unchecked(STD_STR_VALIDATIONS_FILE, 48, 36);
#else
		pg_unreachable();
#endif
	}
	y = *bytes->ptr++;
	ch = std_str_utf8_acc_cont_byte(init, y);
	if (x >= 0xE0)
	{
		uint8		z;
		uint32		y_z;

		if (bytes->ptr == bytes->end)
		{
#if STD_STR_UB_CHECKS && defined(USE_ASSERT_CHECKING)
			std_hint_unreachable_unchecked(STD_STR_VALIDATIONS_FILE, 55, 40);
#else
			pg_unreachable();
#endif
		}
		z = *bytes->ptr++;
		y_z = std_str_utf8_acc_cont_byte((uint32) (y & STD_STR_CONT_MASK), z);
		ch = init << 12 | y_z;
		if (x >= 0xF0)
		{
			uint8		w;

			if (bytes->ptr == bytes->end)
			{
#if STD_STR_UB_CHECKS && defined(USE_ASSERT_CHECKING)
				std_hint_unreachable_unchecked(STD_STR_VALIDATIONS_FILE, 63, 44);
#else
				pg_unreachable();
#endif
			}
			w = *bytes->ptr++;
			ch = (init & 7) << 18 | std_str_utf8_acc_cont_byte(y_z, w);
		}
	}
	*result = ch;
	return true;
}

/*
 * Chars_next is Chars::next: the next code point, made a char by
 * char::from_u32_unchecked, whose check of a valid char value runs in the
 * checked instance.
 */
STD_STR_DEF bool
STD_STR_NAME(Chars_next) (StdChars *self, uint32 *result)
{
	uint32		ch;

	if (!STD_STR_NAME(next_code_point) (&self->iter, &ch))
		return false;
#if STD_STR_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	if (!std_char_try_from_u32_is_ok(ch))
		STD_PTR_VIOLATED("invalid value for `char`", STD_STR_CHAR_METHODS_FILE, 243, 18);
#endif
	*result = ch;
	return true;
}

#endif							/* STD_STR_DEFINE */

#undef STD_STR_CAT_
#undef STD_STR_CAT
#undef STD_STR_DEF_static
#undef STD_STR_DEF_extern
#undef STD_STR_DEF
#undef STD_STR_NAME
#undef STD_STR_SCOPE
#undef STD_STR_DECLARE
#undef STD_STR_DEFINE
#undef STD_STR_UB_CHECKS
#undef STD_STR_SUFFIX
