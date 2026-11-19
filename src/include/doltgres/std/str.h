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
 * The C forms of Rust's &str held as a value, Cow<'_, str>, and the slices
 * of bytes held as values (the port text's sections 4, 5, and 8).
 */
#ifndef DOLTGRES_STD_STR_H
#define DOLTGRES_STD_STR_H

#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/vec.h"

#include "doltgres/std/vec.h"

/*
 * StdStr is a &str or &[u8] held as a value, in a field, an Option, or a
 * slice of them: its bytes, never NULL but for None, and its length.
 */
typedef struct StdStr
{
	const char *ptr;
	size_t		len;
} StdStr;

/*
 * StdBytes is a &[u8] held as a value: its bytes, never NULL but for the
 * None of an Option<&[u8]>, and its length.
 */
typedef struct StdBytes
{
	const uint8 *ptr;
	size_t		len;
} StdBytes;

/* STD_STR is a &str of a literal. */
#define STD_STR(literal) ((StdStr) {(literal), sizeof(literal) - 1})

/* StdCowStrTag is Cow<'_, str>'s variants. */
typedef enum StdCowStrTag
{
	STD_COW_STR_BORROWED,
	STD_COW_STR_OWNED,
} StdCowStrTag;

/* StdCowStr is a Cow<'_, str>: a borrowed str or an owned String. */
typedef struct StdCowStr
{
	StdCowStrTag tag;
	union
	{
		StdStr		borrowed;
		StdString	owned;
	}			u;
} StdCowStr;

/* std_cow_str_deref is Cow's Deref, the str it holds either way. */
static inline StdStr
std_cow_str_deref(const StdCowStr *self)
{
	if (self->tag == STD_COW_STR_BORROWED)
		return self->u.borrowed;
	return (StdStr)
	{
		self->u.owned.ptr, self->u.owned.len
	};
}

/* std_cow_str_drop is Cow's drop, which frees an owned String. */
static inline void
std_cow_str_drop(StdCowStr *self)
{
	if (self->tag == STD_COW_STR_OWNED)
		std_vec_free_Checked(&self->u.owned, 1, 1);
}

/* StdUtf8Error is core's Utf8Error: where the valid prefix ends, and the invalid sequence's length. */
typedef struct StdUtf8Error
{
	size_t		valid_up_to;
	bool		has_error_len;
	uint8		error_len;
} StdUtf8Error;

struct StdFormatter;

/* StdParseIntError is ParseIntError: its kind. */
typedef struct StdParseIntError
{
	StdIntErrorKind kind;
} StdParseIntError;

extern bool std_str_from_utf8(const uint8 *v, size_t len, StdUtf8Error *error);
extern size_t std_str_utf8_char_width(uint8 first);
extern size_t std_str_floor_char_boundary(const char *self, size_t self_len, size_t index);
extern bool std_str_parse_u32(const char *src, size_t len, uint32 *result,
							  StdParseIntError *error);
extern bool std_str_parse_u64(const char *src, size_t len, uint64 *result,
							  StdParseIntError *error);
extern bool std_char_to_digit(uint32 self, uint32 radix, uint32 *result);
extern bool std_u8_from_str_radix(const char *src, size_t len, uint32 radix, uint8 *result,
								  StdParseIntError *error);
extern bool std_utf8_next_chunk(const uint8 *s, size_t len, size_t *valid,
								size_t *inspected);
extern void std_string_from_utf8_lossy(const uint8 *s, size_t len, StdCowStr *result);
extern void std_CowStr_into_owned(StdCowStr *self, StdString *result);
extern void std_CowStr_drop(StdCowStr *self);
extern void std_String_drop(void *self);
extern bool std_String_PartialOrd_lt(const StdString *self, const StdString *other);
extern bool std_Utf8Error_Debug_fmt(const void *value, struct StdFormatter *f);
extern bool std_Utf8Error_error_len_Debug_fmt(const void *value,
											  struct StdFormatter *f);

/* StdSliceIterU8 is slice::Iter<u8>: its next byte and its end. */
typedef struct StdSliceIterU8
{
	const uint8 *ptr;
	const uint8 *end;
} StdSliceIterU8;

/* StdChars is str::Chars: an iterator over a str's bytes, which str_template.h's Chars_next decodes. */
typedef struct StdChars
{
	StdSliceIterU8 iter;
} StdChars;

/* STD_STR_CONT_MASK is validations' CONT_MASK, a continuation byte's value bits. */
#define STD_STR_CONT_MASK 0x3F

/* The Rust files a str's slicing checks name. */
/* StdCharSearcher is pattern.rs's CharSearcher: the haystack, its fingers, and the char's UTF-8. */
typedef struct StdCharSearcher
{
	const uint8 *haystack;
	size_t		haystack_len;
	size_t		finger;
	size_t		finger_back;
	uint8		utf8_size;
	uint8		utf8_encoded[4];
} StdCharSearcher;

/* StdSplitInclusiveChar is SplitInclusive of a char: SplitInternal's range, searcher, and whether it finished. */
typedef struct StdSplitInclusiveChar
{
	size_t		start;
	size_t		end;
	StdCharSearcher matcher;
	bool		finished;
} StdSplitInclusiveChar;

#define STD_CONTAINS_DECLARE
#define STD_CONTAINS_WITH_CONTAINS
#define STD_CONTAINS_SUFFIX Checked
#include "doltgres/std/str_contains_template.h"
#define STD_CONTAINS_DECLARE
#define STD_CONTAINS_WITH_SPLIT
#define STD_CONTAINS_SUFFIX Unchecked
#include "doltgres/std/str_contains_template.h"

extern size_t std_str_Chars_count_Unchecked(const char *s, size_t len);
extern size_t std_str_Chars_count_Checked(const char *s, size_t len);

#define STD_STR_MOD_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "str" STD_SEP "mod.rs"

/* The Rust files str_template.h's checks name. */
#define STD_STR_VALIDATIONS_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "str" STD_SEP "validations.rs"
#define STD_STR_CHAR_METHODS_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "char" STD_SEP "methods.rs"

extern StdChars std_str_chars(const char *s, size_t len);
extern uint32 std_str_utf8_first_byte(uint8 byte, uint32 width);
extern uint32 std_str_utf8_acc_cont_byte(uint32 ch, uint8 byte);
extern bool std_char_try_from_u32_is_ok(uint32 i);
extern size_t std_char_len_utf8(uint32 code);
extern size_t std_str_count_count_chars(const char *s, size_t s_len);
pg_noreturn extern void std_str_slice_error_fail(const char *s, size_t s_len, size_t begin, size_t end,
												 const char *file, uint32 line, uint32 col);
extern void std_str_index_range(const char *s, size_t s_len, size_t start, size_t end, const char *file,
								uint32 line, uint32 col);
extern void std_str_get_unchecked_range_check(size_t start, size_t end, size_t len, const char *file, uint32 line,
											  uint32 col);
extern void std_str_index_range_to(const char *s, size_t s_len, size_t end, const char *file, uint32 line,
								   uint32 col);
extern void std_str_index_range_from(const char *s, size_t s_len, size_t start, const char *file, uint32 line,
									 uint32 col);

extern void test_std_str_tests_a_lossy_conversion_whose_growth_fails_leaks_nothing(void);

#endif							/* DOLTGRES_STD_STR_H */
