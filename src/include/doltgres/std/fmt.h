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
 * The C form of Rust's core::fmt and alloc::fmt::format (the port text's
 * section 9), a port of their code at Rust 1.99.1: a formatter over a
 * fmt::Write, the padding of integers and strings, the integers' Display,
 * hex, and Debug, the Debug builders, and format!, which writes through the
 * same calls as the Rust's, so a String grows where the Rust's grows.
 */
#ifndef DOLTGRES_STD_FMT_H
#define DOLTGRES_STD_FMT_H

#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

/* StdFmtWriteOps is fmt::Write's table; each returns true for Ok. */
typedef struct StdFmtWriteOps
{
	bool		(*write_str) (void *self, const char *s, size_t len);
	bool		(*write_char) (void *self, uint32 c);
} StdFmtWriteOps;

/* StdFmtAlign is fmt::Alignment, with STD_FMT_ALIGN_UNKNOWN for none given. */
typedef enum StdFmtAlign
{
	STD_FMT_ALIGN_LEFT,
	STD_FMT_ALIGN_RIGHT,
	STD_FMT_ALIGN_CENTER,
	STD_FMT_ALIGN_UNKNOWN,
} StdFmtAlign;

/*
 * StdFmtSpec is a placeholder's options, FormattingOptions: its flags, fill,
 * alignment, width, and precision, as {:...} gives them.
 */
typedef struct StdFmtSpec
{
	bool		sign_plus;
	bool		sign_minus;
	bool		alternate;
	bool		sign_aware_zero_pad;
	bool		debug_lower_hex;
	bool		debug_upper_hex;
	uint32		fill;
	StdFmtAlign align;
	bool		has_width;
	uint16		width;
	bool		has_precision;
	uint16		precision;
} StdFmtSpec;

/* STD_FMT_DEFAULT is the options of a plain placeholder, {} or {:?}. */
#define STD_FMT_DEFAULT ((StdFmtSpec) {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN})

/* StdFormatter is a Formatter: where it writes and the placeholder's options. */
typedef struct StdFormatter
{
	const StdFmtWriteOps *ops;
	void	   *buf;
	StdFmtSpec	options;
} StdFormatter;

/* StdFmtFn is a formatting trait's fmt of a value, Display or Debug or kin. */
typedef bool (*StdFmtFn) (const void *value, StdFormatter *f);

/*
 * StdFmtPiece is a piece of format_args!'s template: a literal, or a
 * placeholder of an argument with its options.
 */
typedef struct StdFmtPiece
{
	/* literal is the piece's text, or NULL for a placeholder. */
	const char *literal;
	size_t		literal_len;
	/* arg is the placeholder's argument's index. */
	size_t		arg;
	StdFmtSpec	spec;
} StdFmtPiece;

/* StdFmtArg is an argument of format_args!: a value and how it formats. */
typedef struct StdFmtArg
{
	const void *value;
	StdFmtFn	fmt;
} StdFmtArg;

/* StdFmtArguments is fmt::Arguments: the template's pieces and the arguments. */
typedef struct StdFmtArguments
{
	const StdFmtPiece *pieces;
	size_t		pieces_len;
	const StdFmtArg *args;
} StdFmtArguments;

/* StdNumfmtFormatted is num::fmt::Formatted, which flt2dec.h defines. */
struct StdNumfmtFormatted;

/* STD_FMT_LIT is a literal piece of a template. */
#define STD_FMT_LIT(literal) {(literal), sizeof(literal) - 1, 0, {0}}

/* STD_FMT_ARG is a placeholder of an argument with default options. */
#define STD_FMT_ARG(index) {NULL, 0, (index), {.fill = ' ', .align = STD_FMT_ALIGN_UNKNOWN}}

/* STD_STRING_WRITE_OPS is String's fmt::Write. */
extern const StdFmtWriteOps STD_STRING_WRITE_OPS;

extern bool std_fmt_write_str(StdFormatter *f, const char *s, size_t len);
extern bool std_fmt_write_char(StdFormatter *f, uint32 c);
extern size_t std_fmt_encode_utf8(uint32 c, char *buf);
extern bool std_fmt_Write_write_char(bool (*write_str) (void *self, const char *s, size_t len), void *self,
									 uint32 c);
extern bool std_fmt_write(const StdFmtWriteOps *ops, void *output,
						  const StdFmtArguments *args);
extern bool std_fmt_write_fmt_Checked(StdFormatter *f, const StdFmtArguments *args);
extern bool std_fmt_write_fmt_Unchecked(StdFormatter *f, const StdFmtArguments *args);
extern bool std_fmt_as_str_Unchecked(const StdFmtArguments *args, const char **s, size_t *len);
extern void std_fmt_format_Checked(StdString *result, const StdFmtArguments *args);
extern void std_fmt_format_Unchecked(StdString *result, const StdFmtArguments *args);
extern void std_to_string(const void *value, StdFmtFn display, StdString *result);
extern void std_u32_to_string(uint32 self, StdString *result);
extern bool std_fmt_pad_integral(StdFormatter *f, bool is_nonnegative,
								 const char *prefix, const char *buf,
								 size_t len);
extern bool std_fmt_pad(StdFormatter *f, const char *s, size_t len);
extern bool std_fmt_padding(StdFormatter *f, uint16 pad, StdFmtAlign defaulted, uint32 *post_fill,
							uint16 *post_padding);
extern bool std_fmt_post_padding(StdFormatter *f, uint32 fill, uint16 pad);
extern bool std_fmt_pad_formatted_parts(StdFormatter *f, const struct StdNumfmtFormatted *formatted);
extern bool std_fmt_write_formatted_parts(StdFormatter *f, const struct StdNumfmtFormatted *formatted);
extern bool std_f32_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_f64_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_f64_Display_fmt(const void *value, StdFormatter *f);

/* The formatting traits of the primitive types; each takes a pointer to the value. */
extern bool std_u8_Display_fmt(const void *value, StdFormatter *f);
extern bool std_u32_Display_fmt(const void *value, StdFormatter *f);
extern bool std_u64_Display_fmt(const void *value, StdFormatter *f);
extern bool std_usize_Display_fmt(const void *value, StdFormatter *f);
extern bool std_i16_Display_fmt(const void *value, StdFormatter *f);
extern bool std_i32_Display_fmt(const void *value, StdFormatter *f);
extern bool std_i64_Display_fmt(const void *value, StdFormatter *f);
extern bool std_u8_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_u16_Display_fmt(const void *value, StdFormatter *f);
extern bool std_u16_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_u32_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_u64_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_u128_Display_fmt(const void *value, StdFormatter *f);
extern bool std_u128_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_CellI32_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_NonZeroU64_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_usize_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_i16_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_i32_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_i64_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_u64_LowerHex_fmt(const void *value, StdFormatter *f);
extern bool std_u64_UpperHex_fmt(const void *value, StdFormatter *f);
extern bool std_u8_LowerHex_fmt(const void *value, StdFormatter *f);
extern bool std_u8_UpperHex_fmt(const void *value, StdFormatter *f);
extern bool std_u32_LowerHex_fmt(const void *value, StdFormatter *f);
extern bool std_u32_UpperHex_fmt(const void *value, StdFormatter *f);
extern bool std_fmt_TryFromIntError_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_u16_LowerHex_fmt(const void *value, StdFormatter *f);
extern bool std_bool_Display_fmt(const void *value, StdFormatter *f);
extern bool std_bool_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_str_Display_fmt(const void *value, StdFormatter *f);
extern bool std_String_Display_fmt(const void *value, StdFormatter *f);
extern bool std_str_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_String_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_bytes_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_ptr_Debug_fmt(const void *value, StdFormatter *f);

/* StdDebugStruct is Formatter::debug_struct's builder. */
typedef struct StdDebugStruct
{
	StdFormatter *fmt;
	bool		result;
	bool		has_fields;
} StdDebugStruct;

/* StdDebugTuple is Formatter::debug_tuple's builder. */
typedef struct StdDebugTuple
{
	StdFormatter *fmt;
	bool		result;
	size_t		fields;
	bool		empty_name;
} StdDebugTuple;

/* StdDebugList is Formatter::debug_list's builder, also debug_set's inner. */
typedef struct StdDebugList
{
	StdFormatter *fmt;
	bool		result;
	bool		has_fields;
} StdDebugList;

/* StdDebugMap is Formatter::debug_map's builder, with its PadAdapter's state's on_newline. */
typedef struct StdDebugMap
{
	StdFormatter *fmt;
	bool		result;
	bool		has_fields;
	bool		has_key;
	bool		on_newline;
} StdDebugMap;

extern StdDebugList std_fmt_debug_set(StdFormatter *f);
extern void std_DebugSet_entry(StdDebugList *self, const void *value, StdFmtFn fmt);
extern bool std_DebugSet_finish(StdDebugList *self);
extern StdDebugMap std_fmt_debug_map(StdFormatter *f);
extern void std_DebugMap_entry(StdDebugMap *self, const void *key, StdFmtFn key_fmt, const void *value,
							   StdFmtFn value_fmt);
extern bool std_DebugMap_finish(StdDebugMap *self);

extern StdDebugStruct std_fmt_debug_struct(StdFormatter *f, const char *name,
										   size_t len);
extern void std_DebugStruct_field(StdDebugStruct *self, const char *name,
								  size_t len, const void *value, StdFmtFn fmt);
extern bool std_DebugStruct_finish(StdDebugStruct *self);
extern bool std_DebugStruct_finish_non_exhaustive(StdDebugStruct *self);
extern bool std_unit_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_any_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_TryFromSliceError_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_IntErrorKind_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_Ordering_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_ParseIntError_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_TryFromIntError_Debug_fmt(const void *value, StdFormatter *f);
extern StdDebugTuple std_fmt_debug_tuple(StdFormatter *f, const char *name,
										 size_t len);
extern void std_DebugTuple_field(StdDebugTuple *self, const void *value,
								 StdFmtFn fmt);
extern bool std_DebugTuple_finish(StdDebugTuple *self);
extern StdDebugList std_fmt_debug_list(StdFormatter *f);
extern void std_DebugList_entry(StdDebugList *self, const void *value,
								StdFmtFn fmt);
extern bool std_DebugList_finish(StdDebugList *self);

/* StdDebugRef is a value to format and its Debug, an item of a tuple's or an Option's Debug. */
typedef struct StdDebugRef
{
	const void *value;
	StdFmtFn	fmt;
} StdDebugRef;

/* STD_TUPLE_MAX is the most items a tuple's Debug here takes. */
#define STD_TUPLE_MAX 6

/* StdTupleRef is a tuple to format by its items, as its Debug sees them. */
typedef struct StdTupleRef
{
	size_t		len;
	StdDebugRef items[STD_TUPLE_MAX];
} StdTupleRef;

/* StdOptionRef is an Option to format: whether it has a value, and the value. */
typedef struct StdOptionRef
{
	bool		has;
	StdDebugRef some;
} StdOptionRef;

extern bool std_tuple_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_option_Debug_fmt(const void *value, StdFormatter *f);
pg_noreturn extern void std_assert_failed(const char *file, uint32 line, uint32 col, const char *op,
										  const void *left, StdFmtFn left_fmt,
										  const void *right, StdFmtFn right_fmt, const StdFmtArguments *args);

#endif							/* DOLTGRES_STD_FMT_H */
