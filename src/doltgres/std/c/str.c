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
 * The C forms of core's str functions (the port text's section 9), ported
 * from core/src/str at Rust 1.99.1: from_utf8's validation, without the
 * ASCII block fast path, which changes no result.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#include "port/pg_bitutils.h"

#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/str.h"
#ifdef DOLTGRES_TEST_HOST
#include "doltgres/std/alloc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/unwind.h"
#endif
#include "doltgres/std/unicode.h"
#include "doltgres/std/vec.h"

/* str::Chars::next as core's own code runs it, with no precondition checks (std/str_template.h), for every file. */
#define STD_STR_SCOPE extern
#define STD_STR_DECLARE
#define STD_STR_DEFINE
#define STD_STR_UB_CHECKS 0
#define STD_STR_SUFFIX Unchecked
#include "doltgres/std/str_template.h"

/* str::Chars::next as the crates' code runs it, for contains' checked instance. */
#define STD_STR_SCOPE static
#define STD_STR_DECLARE
#define STD_STR_DEFINE
#define STD_STR_UB_CHECKS 1
#define STD_STR_SUFFIX Checked
#include "doltgres/std/str_template.h"

/* The files str::contains' checks name. */
#define STD_STR_PATTERN_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "str" STD_SEP "pattern.rs"
#define STD_SLICE_MEMCHR_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "slice" STD_SEP "memchr.rs"
#define STD_CHAR_METHODS_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "char" STD_SEP "methods.rs"
#define STD_STR_TRAITS_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "str" STD_SEP "traits.rs"
#define STD_SLICE_CMP_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "slice" STD_SEP "cmp.rs"

/* str::contains of a char as the crates' code runs it, checked in the cassert build. */
#define STD_CONTAINS_DEFINE
#define STD_CONTAINS_WITH_CONTAINS
#define STD_CONTAINS_UB_CHECKS 1
#define STD_CONTAINS_SUFFIX Checked
#include "doltgres/std/str_contains_template.h"

/* split_inclusive and ends_with of a char as core's own Debug builders run them, unchecked. */
#define STD_CONTAINS_DEFINE
#define STD_CONTAINS_WITH_SPLIT
#define STD_CONTAINS_UB_CHECKS 0
#define STD_CONTAINS_SUFFIX Unchecked
#include "doltgres/std/str_contains_template.h"

/* STD_STR_VALIDATIONS_CORE_FILE is core's str/validations.rs, where from_utf8's checks report. */
#define STD_STR_VALIDATIONS_CORE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "str" STD_SEP "validations.rs"

/* STD_STR_UTF8_CHAR_WIDTH is core's UTF8_CHAR_WIDTH: each first byte's sequence length, 0 for none. */
static const uint8 STD_STR_UTF8_CHAR_WIDTH[256] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* std_str_utf8_char_width is core's utf8_char_width: UTF8_CHAR_WIDTH's entry for a first byte. */
size_t
std_str_utf8_char_width(uint8 first)
{
	std_index_check(first, lengthof(STD_STR_UTF8_CHAR_WIDTH), STD_STR_VALIDATIONS_CORE_FILE, 279, 5);
	return STD_STR_UTF8_CHAR_WIDTH[first];
}

/* std_str_contains_nonascii is validations.rs's contains_nonascii: whether a word has a byte of 128 or more. */
static bool
std_str_contains_nonascii(size_t x)
{
	const size_t NONASCII_MASK = (size_t) UINT64CONST(0x8080808080808080);

	return (x & NONASCII_MASK) != 0;
}

/*
 * std_str_from_utf8 is str::from_utf8, core's run_utf8_validation: true for
 * valid UTF-8, and otherwise the Utf8Error where the valid prefix ends and
 * the length of the invalid sequence, none for one cut short by the end;
 * ASCII runs are skipped two words at a time from an aligned index.
 */
bool
std_str_from_utf8(const uint8 *v, size_t len, StdUtf8Error *error)
{
	const size_t USIZE_BYTES = sizeof(size_t);
	size_t		index = 0;
	size_t		ascii_block_size = 2 * USIZE_BYTES;
	size_t		blocks_end = len >= ascii_block_size ? len - ascii_block_size + 1 : 0;
	size_t		align = std_ptr_const_align_offset(v, 1, USIZE_BYTES);

	while (index < len)
	{
		size_t		old_offset = index;
		uint8		first;

#define UTF8_ERR(has, n) \
	do { error->valid_up_to = old_offset; error->has_error_len = (has); \
		 error->error_len = (n); return false; } while (0)
#define UTF8_NEXT(byte, line, col) \
	do { index++; if (index >= len) UTF8_ERR(false, 0); \
		 std_index_check(index, len, STD_STR_VALIDATIONS_CORE_FILE, (line), (col)); (byte) = v[index]; } while (0)

		std_index_check(index, len, STD_STR_VALIDATIONS_CORE_FILE, 164, 21);
		first = v[index];
		if (first >= 128)
		{
			uint8		second;
			uint8		next;

			switch (std_str_utf8_char_width(first))
			{
				case 2:
					UTF8_NEXT(next, 187, 24);
					if ((int8) next >= -64)
						UTF8_ERR(true, 1);
					break;
				case 3:
					UTF8_NEXT(second, 192, 35);
					if (!((first == 0xE0 && second >= 0xA0 && second <= 0xBF) ||
						  (first >= 0xE1 && first <= 0xEC && second >= 0x80 && second <= 0xBF) ||
						  (first == 0xED && second >= 0x80 && second <= 0x9F) ||
						  (first >= 0xEE && first <= 0xEF && second >= 0x80 && second <= 0xBF)))
						UTF8_ERR(true, 1);
					UTF8_NEXT(next, 199, 24);
					if ((int8) next >= -64)
						UTF8_ERR(true, 2);
					break;
				case 4:
					UTF8_NEXT(second, 204, 35);
					if (!((first == 0xF0 && second >= 0x90 && second <= 0xBF) ||
						  (first >= 0xF1 && first <= 0xF3 && second >= 0x80 && second <= 0xBF) ||
						  (first == 0xF4 && second >= 0x80 && second <= 0x8F)))
						UTF8_ERR(true, 1);
					UTF8_NEXT(next, 208, 24);
					if ((int8) next >= -64)
						UTF8_ERR(true, 2);
					UTF8_NEXT(next, 211, 24);
					if ((int8) next >= -64)
						UTF8_ERR(true, 3);
					break;
				default:
					UTF8_ERR(true, 1);
			}
			index++;
		}
		else
		{
			if (align != SIZE_MAX &&
				std_is_multiple_of_usize(align - index, USIZE_BYTES))
			{
				while (index < blocks_end)
				{
					size_t		zu_word;
					size_t		zv_word;

					memcpy(&zu_word, v + index, sizeof(size_t));
					memcpy(&zv_word, v + index + sizeof(size_t), sizeof(size_t));
					if (std_str_contains_nonascii(zu_word) || std_str_contains_nonascii(zv_word))
						break;
					index += ascii_block_size;
				}
				while (index < len)
				{
					std_index_check(index, len, STD_STR_VALIDATIONS_CORE_FILE, 241, 38);
					if (!(v[index] < 128))
						break;
					index += 1;
				}
			}
			else
				index++;
		}
#undef UTF8_NEXT
#undef UTF8_ERR
	}
	return true;
}

/* std_Utf8Error_Debug_fmt is Utf8Error's derived Debug. */
bool
std_Utf8Error_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdUtf8Error *self = (const StdUtf8Error *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Utf8Error", 9);

	std_DebugStruct_field(&b, "valid_up_to", 11, &self->valid_up_to, std_usize_Debug_fmt);
	std_DebugStruct_field(&b, "error_len", 9, self, std_Utf8Error_error_len_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* std_Utf8Error_error_len_Debug_fmt is the Debug of Utf8Error's Option<u8> field. */
bool
std_Utf8Error_error_len_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdUtf8Error *self = (const StdUtf8Error *) value;
	StdDebugTuple b;

	if (!self->has_error_len)
		return std_fmt_write_str(f, "None", 4);
	b = std_fmt_debug_tuple(f, "Some", 4);
	std_DebugTuple_field(&b, &self->error_len, std_u8_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* std_str_utf8_width is core's width of a UTF-8 sequence by its first byte, 0 for none. */
static size_t
std_str_utf8_width(uint8 b)
{
	if (b >= 0xC2 && b <= 0xDF)
		return 2;
	if (b >= 0xE0 && b <= 0xEF)
		return 3;
	if (b >= 0xF0 && b <= 0xF4)
		return 4;
	return 0;
}

/* std_str_safe_get is a byte of a slice, or 0 past its end. */
static uint8
std_str_safe_get(const uint8 *s, size_t len, size_t i)
{
	return i < len ? s[i] : 0;
}

/*
 * std_utf8_next_chunk is Utf8Chunks::next: the length of the valid run at the start of
 * the source and how far it inspected, none for an empty source.
 */
bool
std_utf8_next_chunk(const uint8 *s, size_t len, size_t *valid, size_t *inspected)
{
	size_t		i = 0;
	size_t		valid_up_to = 0;

	if (len == 0)
		return false;
	while (i < len)
	{
		uint8		byte = s[i];

		i++;
		if (byte >= 128)
		{
			size_t		w = std_str_utf8_width(byte);

			if (w == 2)
			{
				if ((std_str_safe_get(s, len, i) & 192) != 128)
					break;
				i++;
			}
			else if (w == 3)
			{
				uint8		b1 = std_str_safe_get(s, len, i);

				if (!((byte == 0xE0 && b1 >= 0xA0 && b1 <= 0xBF) ||
					  (byte >= 0xE1 && byte <= 0xEC && b1 >= 0x80 && b1 <= 0xBF) ||
					  (byte == 0xED && b1 >= 0x80 && b1 <= 0x9F) ||
					  (byte >= 0xEE && byte <= 0xEF && b1 >= 0x80 && b1 <= 0xBF)))
					break;
				i++;
				if ((std_str_safe_get(s, len, i) & 192) != 128)
					break;
				i++;
			}
			else if (w == 4)
			{
				uint8		b1 = std_str_safe_get(s, len, i);

				if (!((byte == 0xF0 && b1 >= 0x90 && b1 <= 0xBF) ||
					  (byte >= 0xF1 && byte <= 0xF3 && b1 >= 0x80 && b1 <= 0xBF) ||
					  (byte == 0xF4 && b1 >= 0x80 && b1 <= 0x8F)))
					break;
				i++;
				if ((std_str_safe_get(s, len, i) & 192) != 128)
					break;
				i++;
				if ((std_str_safe_get(s, len, i) & 192) != 128)
					break;
				i++;
			}
			else
				break;
		}
		valid_up_to = i;
	}
	*valid = valid_up_to;
	*inspected = i;
	return true;
}

/*
 * std_string_from_utf8_lossy is String::from_utf8_lossy: the bytes as text,
 * borrowed when they are valid UTF-8, and otherwise owned with U+FFFD in
 * place of each invalid sequence.
 */
void
std_string_from_utf8_lossy(const uint8 *s, size_t len, StdCowStr *result)
{
	static const char replacement[3] = {(char) 0xEF, (char) 0xBF, (char) 0xBD};
	size_t		valid;
	size_t		inspected;
	size_t		at;
	size_t		total;
	size_t		next_valid;
	size_t		next_inspected;
	StdString	res;

	if (!std_utf8_next_chunk(s, len, &valid, &inspected))
	{
		result->tag = STD_COW_STR_BORROWED;
		result->u.borrowed = (StdStr)
		{
			"", 0
		};
		return;
	}
	if (inspected == valid)
	{
		result->tag = STD_COW_STR_BORROWED;
		result->u.borrowed = (StdStr)
		{
			(const char *) s, len
		};
		return;
	}
	total = valid + 3;
	at = inspected;
	while (std_utf8_next_chunk(s + at, len - at, &next_valid, &next_inspected))
	{
		total += next_valid + (next_inspected != next_valid ? 3 : 0);
		at += next_inspected;
	}
	std_vec_with_capacity_Unchecked(&res, total, 1, 1);
	std_string_push_str_Unchecked(&res, (const char *) s, valid);
	std_string_push_str_Unchecked(&res, replacement, 3);
	at = inspected;
	while (std_utf8_next_chunk(s + at, len - at, &valid, &inspected))
	{
		std_string_push_str_Unchecked(&res, (const char *) s + at, valid);
		if (inspected != valid)
			std_string_push_str_Unchecked(&res, replacement, 3);
		at += inspected;
	}
	result->tag = STD_COW_STR_OWNED;
	result->u.owned = res;
}

/* std_CowStr_into_owned is Cow<str>::into_owned: the owned String, or the borrowed text copied into one; it takes the Cow. */
void
std_CowStr_into_owned(StdCowStr *self, StdString *result)
{
	switch (self->tag)
	{
		case STD_COW_STR_BORROWED:
			std_string_from_Checked(result, self->u.borrowed.ptr, self->u.borrowed.len);
			return;
		case STD_COW_STR_OWNED:
			*result = self->u.owned;
			return;
	}
	pg_unreachable();
}

/* std_CowStr_drop is Cow<str>'s drop: an owned String's buffer freed. */
void
std_CowStr_drop(StdCowStr *self)
{
	if (self->tag == STD_COW_STR_OWNED)
		std_vec_free_Checked(&self->u.owned, 1, 1);
}

/* std_String_drop is String's drop glue, which a Vec<String>'s element drop reaches through its pointer. */
void
std_String_drop(void *self)
{
	std_vec_free_Checked(self, 1, 1);
}

/*
 * std_String_PartialOrd_lt is String's derived PartialOrd::lt, on pointers
 * to the values: its bytes' partial_cmp, [u8]'s SliceOrd, is Less.
 */
bool
std_String_PartialOrd_lt(const StdString *self, const StdString *other)
{
	return std_bytes_Ord_cmp(self->ptr, self->len, other->ptr, other->len) == STD_ORDERING_LESS;
}

/*
 * std_str_parse_u32 is u32's FromStr, core's from_ascii_bytes_radix_impl at
 * radix 10: an optional '+', at least one digit, and nothing else, checked
 * for overflow only past 8 digits, as can_not_overflow allows.
 */
bool
std_str_parse_u32(const char *src, size_t len, uint32 *result, StdParseIntError *error)
{
	const uint8 *digits = (const uint8 *) src;
	size_t		left = len;
	uint32		value = 0;

	if (len == 0)
	{
		error->kind = STD_INT_ERROR_KIND_EMPTY;
		return false;
	}
	if (len == 1 && (digits[0] == '+' || digits[0] == '-'))
	{
		error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
		return false;
	}
	if (digits[0] == '+')
	{
		digits++;
		left--;
	}
	if (left <= sizeof(uint32) * 2)
	{
		for (; left > 0; digits++, left--)
		{
			value = value * 10;
			if (!(*digits >= '0' && *digits <= '9'))
			{
				error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
				return false;
			}
			value = value + (uint32) (*digits - '0');
		}
	}
	else
	{
		for (; left > 0; digits++, left--)
		{
			uint32		mul;
			bool		has_mul = std_checked_mul_u32(value, 10, &mul);

			if (!(*digits >= '0' && *digits <= '9'))
			{
				error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
				return false;
			}
			if (!has_mul)
			{
				error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
				return false;
			}
			value = mul;
			if (!std_checked_add_u32(value, (uint32) (*digits - '0'), &value))
			{
				error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
				return false;
			}
		}
	}
	*result = value;
	return true;
}

/*
 * std_str_parse_u64 is u64's FromStr, core's from_ascii_bytes_radix_impl at
 * radix 10: an optional '+', at least one digit, and nothing else, checked
 * for overflow only past 16 digits, as can_not_overflow allows.
 */
bool
std_str_parse_u64(const char *src, size_t len, uint64 *result, StdParseIntError *error)
{
	const uint8 *digits = (const uint8 *) src;
	size_t		left = len;
	uint64		value = 0;

	if (len == 0)
	{
		error->kind = STD_INT_ERROR_KIND_EMPTY;
		return false;
	}
	if (len == 1 && (digits[0] == '+' || digits[0] == '-'))
	{
		error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
		return false;
	}
	if (digits[0] == '+')
	{
		digits++;
		left--;
	}
	if (left <= sizeof(uint64) * 2)
	{
		for (; left > 0; digits++, left--)
		{
			value = value * 10;
			if (!(*digits >= '0' && *digits <= '9'))
			{
				error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
				return false;
			}
			value = value + (uint64) (*digits - '0');
		}
	}
	else
	{
		for (; left > 0; digits++, left--)
		{
			uint64		mul;
			bool		has_mul = std_checked_mul_u64(value, 10, &mul);

			if (!(*digits >= '0' && *digits <= '9'))
			{
				error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
				return false;
			}
			if (!has_mul)
			{
				error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
				return false;
			}
			value = mul;
			if (!std_checked_add_u64(value, (uint64) (*digits - '0'), &value))
			{
				error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
				return false;
			}
		}
	}
	*result = value;
	return true;
}

/*
 * std_char_to_digit is char::to_digit: the char's value as a digit of the
 * radix, A to Z and a to z standing for 10 to 35, or false for a char that
 * is no such digit.
 */
bool
std_char_to_digit(uint32 self, uint32 radix, uint32 *result)
{
	uint32		value;

	if (!(radix >= 2 && radix <= 36))
	{
		static const char text[] = "to_digit: invalid radix -- radix must be in the range 2 to 36 inclusive";

		std_panic(STD_CHAR_METHODS_FILE, 407, 9, text, sizeof(text) - 1);
	}
	if (self > '9' && radix > 10)
		value = std_wrapping_add_u32(std_wrapping_sub_u32(self, 'A') & ~(uint32) 0x20, 10);
	else
		value = std_wrapping_sub_u32(self, '0');
	if (!(value < radix))
		return false;
	*result = value;
	return true;
}

/*
 * std_u8_from_str_radix is u8::from_str_radix, core's
 * from_ascii_bytes_radix_impl: an optional '+', at least one digit of the
 * radix, and nothing else, checked for overflow only past two digits at a
 * radix up to 16, as can_not_overflow allows.
 */
bool
std_u8_from_str_radix(const char *src, size_t len, uint32 radix, uint8 *result, StdParseIntError *error)
{
	const uint8 *digits = (const uint8 *) src;
	size_t		digits_len = len;
	uint8		value = 0;

	if (2 > radix || radix > 36)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("from_ascii_bytes_radix: radix must lie in the range `[2, 36]` - found "),
		STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&radix, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(STD_NUM_MOD_FILE, 1867, 1, &arguments);
	}
	if (len == 0)
	{
		error->kind = STD_INT_ERROR_KIND_EMPTY;
		return false;
	}
	if (len == 1 && (digits[0] == '+' || digits[0] == '-'))
	{
		error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
		return false;
	}
	if (digits[0] == '+')
	{
		digits = STD_PTR_AT(digits, 1, 1);
		digits_len = std_wrapping_sub_usize(digits_len, 1);
	}
	if (radix <= 16 && digits_len <= sizeof(uint8) * 2)
	{
		for (size_t i = 0, i_len = digits_len; i < i_len; i++)
		{
			uint32		x;

			value = std_wrapping_mul_u8(value, (uint8) radix);
			if (!std_char_to_digit(*(const uint8 *) STD_PTR_AT(digits, i, 1), radix, &x))
			{
				error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
				return false;
			}
			value = std_wrapping_add_u8(value, (uint8) x);
		}
	}
	else
	{
		for (size_t i = 0, i_len = digits_len; i < i_len; i++)
		{
			uint8		mul;
			bool		has_mul = std_checked_mul_u8(value, (uint8) radix, &mul);
			uint32		x;

			if (!std_char_to_digit(*(const uint8 *) STD_PTR_AT(digits, i, 1), radix, &x))
			{
				error->kind = STD_INT_ERROR_KIND_INVALID_DIGIT;
				return false;
			}
			if (!has_mul)
			{
				error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
				return false;
			}
			if (!std_checked_add_u8(mul, (uint8) x, &value))
			{
				error->kind = STD_INT_ERROR_KIND_POS_OVERFLOW;
				return false;
			}
		}
	}
	*result = value;
	return true;
}

/* std_str_chars is str::chars: an iterator over the str's chars. */
StdChars
std_str_chars(const char *s, size_t len)
{
	StdChars	chars;

	chars.iter.ptr = (const uint8 *) s;
	chars.iter.end = (const uint8 *) s + len;
	return chars;
}

/* std_str_utf8_first_byte is validations' utf8_first_byte: a lead byte's value bits for a sequence's width. */
uint32
std_str_utf8_first_byte(uint8 byte, uint32 width)
{
	return (uint32) (uint8) (byte & (0x7F >> width));
}

/* std_str_utf8_acc_cont_byte is validations' utf8_acc_cont_byte: a code point with a continuation byte's bits added. */
uint32
std_str_utf8_acc_cont_byte(uint32 ch, uint8 byte)
{
	return (ch << 6) | (uint32) (uint8) (byte & STD_STR_CONT_MASK);
}

/*
 * std_char_try_from_u32_is_ok is whether char_try_from_u32 is Ok: a value
 * below 0x110000 outside the surrogates, tested as core's one comparison.
 */
bool
std_char_try_from_u32_is_ok(uint32 i)
{
	return !((uint32) ((i ^ 0xD800) - 0x800) >= 0x110000 - 0x800);
}

/* std_char_len_utf8 is char::len_utf8: the bytes a code point's UTF-8 takes. */
size_t
std_char_len_utf8(uint32 code)
{
	if (code < 0x80)
		return 1;
	else if (code < 0x800)
		return 2;
	else if (code < 0x10000)
		return 3;
	else
		return 4;
}

/* STD_STR_COUNT_FILE is core's str/count.rs, where count_chars' slice calls report. */
#define STD_STR_COUNT_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "str" STD_SEP "count.rs"

/* std_str_count_char_count_general_case is count.rs's char_count_general_case: the bytes that don't continue a char. */
static size_t
std_str_count_char_count_general_case(const uint8 *s, size_t s_len)
{
	size_t		count = 0;

	for (size_t i = 0; i < s_len; i++)
		if (!((int8) s[i] < -64))
			count++;
	return count;
}

/*
 * std_str_count_contains_non_continuation_byte is count.rs's: a word with a
 * one in each byte's lowest bit whose byte doesn't continue a char.
 */
static size_t
std_str_count_contains_non_continuation_byte(size_t w)
{
	const size_t LSB = (size_t) UINT64CONST(0x0101010101010101);

	return ((~w >> 7) | (w >> 6)) & LSB;
}

/* std_str_count_sum_bytes_in_usize is count.rs's: the sum of a word's bytes. */
static size_t
std_str_count_sum_bytes_in_usize(size_t values)
{
	const size_t LSB_SHORTS = (size_t) UINT64CONST(0x0001000100010001);
	const size_t SKIP_BYTES = (size_t) UINT64CONST(0x00FF00FF00FF00FF);
	size_t		pair_sum = (values & SKIP_BYTES) + ((values >> 8) & SKIP_BYTES);

	return (pair_sum * LSB_SHORTS) >> ((sizeof(size_t) - 2) * 8);
}

/*
 * std_str_count_do_count_chars is count.rs's do_count_chars: a str's chars
 * counted a word at a time over its aligned middle, as align_to splits it,
 * in chunks of 192 words unrolled by 4, and a byte at a time over its ends.
 */
static size_t
std_str_count_do_count_chars(const char *s, size_t s_len)
{
	const size_t CHUNK_SIZE = 192;
	const size_t UNROLL_INNER = 4;
	StdSliceAlignTo split;
	StdChunks	chunks;
	const void *chunk;
	size_t		chunk_len;
	size_t		total;

	std_slice_align_to(s, s_len, 1, 1, sizeof(size_t), _Alignof(size_t), &split);
	if (unlikely(split.body_len == 0 || split.head_len > sizeof(size_t) || split.tail_len > sizeof(size_t)))
		return std_str_count_char_count_general_case((const uint8 *) s, s_len);
	total = std_str_count_char_count_general_case(split.head, split.head_len) +
		std_str_count_char_count_general_case(split.tail, split.tail_len);
	chunks = std_slice_chunks(split.body, split.body_len, CHUNK_SIZE);
	while (std_slice_Chunks_next(&chunks, sizeof(size_t), _Alignof(size_t), &chunk, &chunk_len))
	{
		size_t		counts = 0;
		const void *unrolled_chunks;
		size_t		unrolled_len;
		const void *remainder;
		size_t		remainder_len;

		std_slice_as_chunks(chunk, chunk_len, sizeof(size_t), _Alignof(size_t), UNROLL_INNER, &unrolled_chunks,
							&unrolled_len, &remainder, &remainder_len, STD_STR_COUNT_FILE, 80, 50);
		for (size_t u = 0; u < unrolled_len; u++)
			for (size_t i = 0; i < UNROLL_INNER; i++)
				counts += std_str_count_contains_non_continuation_byte(
																	   ((const size_t *) unrolled_chunks)[u * UNROLL_INNER + i]);
		total += std_str_count_sum_bytes_in_usize(counts);
		if (remainder_len != 0)
		{
			size_t		remainder_counts = 0;

			for (size_t i = 0; i < remainder_len; i++)
				remainder_counts += std_str_count_contains_non_continuation_byte(((const size_t *) remainder)[i]);
			total += std_str_count_sum_bytes_in_usize(remainder_counts);
			break;
		}
	}
	return total;
}

/*
 * std_str_count_count_chars is count.rs's count_chars, which Chars::count
 * is: a short str's chars counted a byte at a time, and a longer one's by
 * do_count_chars.
 */
size_t
std_str_count_count_chars(const char *s, size_t s_len)
{
	if (s_len < sizeof(size_t) * 4)
		return std_str_count_char_count_general_case((const uint8 *) s, s_len);
	else
		return std_str_count_do_count_chars(s, s_len);
}

/*
 * std_str_Chars_count is Chars::count as core's own code runs it: count_chars
 * of as_str, whose slice's length len! takes by NonNull::offset_from_unsigned.
 */
size_t
std_str_Chars_count_Unchecked(const char *s, size_t len)
{
	size_t		n = std_ptr_offset_from_unsigned_Unchecked(s + len, s, 1, STD_PTR_NON_NULL_FILE, 898, 32);

	return std_str_count_count_chars(s, n);
}

/*
 * std_str_Chars_count_Checked is Chars::count as a crate's code runs it, its
 * #[inline] codegened there: len!'s offset_from_unsigned checked, count_chars
 * core's own code.
 */
size_t
std_str_Chars_count_Checked(const char *s, size_t len)
{
	size_t		n = std_ptr_offset_from_unsigned_Checked(s + len, s, 1, STD_PTR_NON_NULL_FILE, 898, 32);

	return std_str_count_count_chars(s, n);
}

/* std_str_is_utf8_char_boundary is u8::is_utf8_char_boundary: whether a byte starts a char or ends a str. */
static bool
std_str_is_utf8_char_boundary(uint8 self)
{
	return (int8) self >= -0x40;
}

/* std_str_is_char_boundary is str::is_char_boundary: whether an index starts a char or is the str's end. */
bool
std_str_is_char_boundary(const char *self, size_t self_len, size_t index)
{
	if (index == 0)
		return true;
	if (index >= self_len)
		return index == self_len;
	else
	{
		std_index_check(index, self_len, STD_STR_MOD_FILE, 396, 13);
		return std_str_is_utf8_char_boundary((uint8) self[index]);
	}
}

/* std_str_floor_char_boundary is str::floor_char_boundary: the closest char boundary at or below an index. */
size_t
std_str_floor_char_boundary(const char *self, size_t self_len, size_t index)
{
	if (index >= self_len)
		return self_len;
	std_index_check(index, self_len, STD_STR_MOD_FILE, 428, 12);
	if (std_str_is_utf8_char_boundary((uint8) self[index]))
		return index;
	std_index_check(index - 1, self_len, STD_STR_MOD_FILE, 440, 12);
	if (std_str_is_utf8_char_boundary((uint8) self[index - 1]))
		return index - 1;
	std_index_check(index - 2, self_len, STD_STR_MOD_FILE, 446, 12);
	if (std_str_is_utf8_char_boundary((uint8) self[index - 2]))
		return index - 2;
	return index - 3;
}

/* std_str_ceil_char_boundary is str::ceil_char_boundary: the closest char boundary at or above an index. */
static size_t
std_str_ceil_char_boundary(const char *self, size_t self_len, size_t index)
{
	size_t		i;

	if (index >= self_len)
		return self_len;
	i = index;
	for (;;)
	{
		std_index_check(i, self_len, STD_STR_MOD_FILE, 486, 20);
		if (std_str_is_utf8_char_boundary((uint8) self[i]))
			break;
		i += 1;
		if (i >= self_len)
			break;
	}
	return i;
}

/*
 * std_str_panic_two panics at a location with panic!'s formatted message of
 * two usizes after two literals, whose payload is a String.
 */
pg_noreturn static void
std_str_panic_two(const char *file, uint32 line, uint32 col, const char *first, size_t a, const char *second, size_t b)
{
	StdFmtPiece pieces[] = {
		{first, strlen(first), 0, {0}}, STD_FMT_ARG(0), {second, strlen(second), 0, {0}}, STD_FMT_ARG(1),
	};
	StdFmtArg	args[] = {{&a, std_usize_Display_fmt}, {&b, std_usize_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_panic_fmt(file, line, col, &arguments);
}

/*
 * std_str_char_Debug_fmt is char's Debug: the char between single quotes,
 * escaped by escape_debug_ext with grapheme extenders and single quotes
 * escaped.
 */
static bool
std_str_char_Debug_fmt(const void *value, StdFormatter *f)
{
	uint32		c = *(const uint32 *) value;
	char		esc[16];
	size_t		esc_len;

	if (!std_fmt_write_char(f, '\''))
		return false;
	if (std_char_escape_debug_ext(c, true, true, false, esc, &esc_len) == 1)
	{
		if (!std_fmt_write_char(f, c))
			return false;
	}
	else if (!std_fmt_write_str(f, esc, esc_len))
		return false;
	return std_fmt_write_char(f, '\'');
}

/* std_str_RangeUsize_Debug_fmt is Range<usize>'s Debug: its start's Debug, "..", and its end's. */
static bool
std_str_RangeUsize_Debug_fmt(const void *value, StdFormatter *f)
{
	const size_t *range = value;

	if (!std_usize_Debug_fmt(&range[0], f))
		return false;
	if (!std_fmt_write_str(f, "..", 2))
		return false;
	return std_usize_Debug_fmt(&range[1], f);
}

/*
 * std_str_slice_error_fail_inside is slice_error_fail_rt's panic for an
 * index inside a char: the char, by its Debug, and the bytes it spans; its
 * slicing and its unwrap of the char report the caller's location, since
 * slice_error_fail_rt is #[track_caller].
 */
pg_noreturn static void
std_str_slice_error_fail_inside(const char *s, size_t s_len, const char *which, size_t index, const char *file,
								uint32 line, uint32 col)
{
	size_t		floor = std_str_floor_char_boundary(s, s_len, index);
	size_t		ceil = std_str_ceil_char_boundary(s, s_len, index);
	StdChars	chars;
	uint32		ch;
	size_t		range[2] = {floor, ceil};
	StdFmtPiece pieces[] = {
		{which, strlen(which), 0, {0}}, STD_FMT_ARG(0), STD_FMT_LIT(" is not a char boundary; it is inside "),
		STD_FMT_ARG(1), STD_FMT_LIT(" (bytes "), STD_FMT_ARG(2), STD_FMT_LIT(" of string)"),
	};
	StdFmtArg	args[] = {{&index, std_usize_Display_fmt}, {&ch, std_str_char_Debug_fmt},
	{range, std_str_RangeUsize_Debug_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_str_index_range(s, s_len, floor, ceil, file, line, col);
	chars = std_str_chars(s + floor, ceil - floor);
	if (!std_str_Chars_next_Unchecked(&chars, &ch))
		std_option_unwrap_failed(file, line, col);
	std_panic_fmt(file, line, col, &arguments);
}

/*
 * std_str_slice_error_fail is str's slice_error_fail, #[track_caller], at
 * run time: the panic of a str sliced out of its bounds, backward, or inside
 * a char, in slice_error_fail_rt's order.
 */
void
std_str_slice_error_fail(const char *s, size_t s_len, size_t begin, size_t end, const char *file, uint32 line, uint32 col)
{
	size_t		len = s_len;

	if (begin > len)
		std_str_panic_two(file, line, col, "start byte index ", begin, " is out of bounds for string of length ", len);
	if (end > len)
		std_str_panic_two(file, line, col, "end byte index ", end, " is out of bounds for string of length ", len);
	if (begin > end)
		std_str_panic_two(file, line, col, "byte range starts at ", begin, " but ends at ", end);
	if (!std_str_is_char_boundary(s, s_len, begin))
		std_str_slice_error_fail_inside(s, s_len, "start byte index ", begin, file, line, col);
	if (!std_str_is_char_boundary(s, s_len, end))
		std_str_slice_error_fail_inside(s, s_len, "end byte index ", end, file, line, col);
	std_str_panic_two(file, line, col, "end byte index ", end, " is out of bounds for string of length ", len);
}

/*
 * std_str_index_range is the check of a str's `s[start..end]`, check_range's:
 * the range ordered, within the str, and on char boundaries, or
 * slice_error_fail's panic at the caller's location, as Index::index is
 * #[track_caller].
 */
void
std_str_index_range(const char *s, size_t s_len, size_t start, size_t end, const char *file, uint32 line,
					uint32 col)
{
	bool		ok;

	if (start > end || end > s_len)
		ok = false;
	else if (start == s_len)
		ok = true;
	else
		ok = (start == 0 || std_str_is_utf8_char_boundary((uint8) s[start])) &&
			(end == s_len || std_str_is_utf8_char_boundary((uint8) s[end]));
	if (!ok)
		std_str_slice_error_fail(s, s_len, start, end, file, line, col);
}

/*
 * std_str_get_unchecked_range_check is str's Range::get_unchecked check,
 * which is #[track_caller]: the range ordered and within the string.
 */
void
std_str_get_unchecked_range_check(size_t start, size_t end, size_t len, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!(end >= start && end <= len))
		STD_PTR_VIOLATED("str::get_unchecked requires that the range is within the string slice", file, line, col);
#endif
}

/*
 * std_str_index_range_to is the check of a str's `s[..end]`: end on a char
 * boundary, or slice_error_fail's panic of 0..end at the caller's location.
 */
void
std_str_index_range_to(const char *s, size_t s_len, size_t end, const char *file, uint32 line, uint32 col)
{
	if (!std_str_is_char_boundary(s, s_len, end))
		std_str_slice_error_fail(s, s_len, 0, end, file, line, col);
}

/*
 * std_str_index_range_from is the check of a str's `s[start..]`: start on a
 * char boundary, or slice_error_fail's panic at the caller's location.
 */
void
std_str_index_range_from(const char *s, size_t s_len, size_t start, const char *file, uint32 line, uint32 col)
{
	if (!std_str_is_char_boundary(s, s_len, start))
		std_str_slice_error_fail(s, s_len, start, s_len, file, line, col);
}

/*
 * std_string_truncate is String::truncate: the string cut to a length no
 * longer than it, asserting at its caller that the cut is a char boundary.
 */
void
std_string_truncate(StdString *self, size_t new_len, const char *file, uint32 line, uint32 col)
{
	if (new_len <= self->len)
	{
		if (!std_str_is_char_boundary(self->ptr, self->len, new_len))
		{
			static const char text[] = "assertion failed: self.is_char_boundary(new_len)";

			std_panic(file, line, col, text, sizeof(text) - 1);
		}
		std_vec_truncate(self, new_len, 1, NULL);
	}
}

#ifdef DOLTGRES_TEST_HOST

/* std_str_tests_oom is the lossy case's allocation error hook: a failed allocation panics, as pg's hook unwinds. */
static void
std_str_tests_oom(size_t size, size_t align)
{
	std_panic(__FILE__, __LINE__, 0, "out of memory", sizeof("out of memory") - 1);
}

/* std_str_tests_OOM_HOOK is the lossy case's hook. */
static const StdAllocErrorHook std_str_tests_OOM_HOOK = {std_str_tests_oom};

/* std_str_tests_lossy is the lossy case's body: an "a" and eight bytes that aren't UTF-8 made a String lossily. */
static void
std_str_tests_lossy(void *captures)
{
	static const uint8 bytes[] = {'a', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	StdCowStr	lossy;

	std_string_from_utf8_lossy(bytes, sizeof(bytes), &lossy);
	std_cow_str_drop(&lossy);
}

/*
 * test_std_str_tests_a_lossy_conversion_whose_growth_fails_leaks_nothing
 * makes a String lossily from bytes whose replacements outgrow them, with
 * each of the global allocator's reallocations failing in turn: no failure
 * leaks the String, as Rust's unwinding drops it.
 */
void
test_std_str_tests_a_lossy_conversion_whose_growth_fails_leaks_nothing(void)
{
	StdOutputCapture *capture = std_io_set_output_capture(NULL);

	for (uint64 nth = 1;; nth++)
	{
		uint64		before = test_std_alloc_tests_in_use();
		StdCaught	caught;
		bool		ok;

		std_alloc_set_alloc_error_hook(&std_str_tests_OOM_HOOK);
		test_std_alloc_tests_fail_reallocation(nth);
		ok = std_catch_unwind(std_str_tests_lossy, NULL, &caught);
		test_std_alloc_tests_fail_reallocation(0);
		std_alloc_set_alloc_error_hook(NULL);
		if (!ok)
			std_Caught_drop(&caught);
		if (test_std_alloc_tests_in_use() != before)
			std_panic(__FILE__, __LINE__, 0, "a failed reallocation of the conversion leaked",
					  sizeof("a failed reallocation of the conversion leaked") - 1);
		if (ok)
			break;
	}
	(void) std_io_set_output_capture(capture);
}

#endif							/* DOLTGRES_TEST_HOST */
