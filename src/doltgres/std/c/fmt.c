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
 * section 9), ported from core/src/fmt/{mod,builders,num}.rs and
 * alloc/src/fmt.rs at Rust 1.99.1. It writes through the same write_str and
 * write_char calls the Rust makes, in the same order, so a String it writes
 * grows where the Rust's does. The Debug builders' alternate form ({:#?})
 * writes through PadAdapter, as builders.rs's does.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/flt2dec.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unicode.h"
#include "doltgres/std/unwind.h"

/* str::Chars::next as core's own code runs it, with no precondition checks, which str.c defines. */
#define STD_STR_SCOPE extern
#define STD_STR_DECLARE
#define STD_STR_UB_CHECKS 0
#define STD_STR_SUFFIX Unchecked
#include "doltgres/std/str_template.h"

/* FMT_FILE is alloc/src/fmt.rs as Rust's panics place it. */
#define FMT_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "fmt.rs"

/* std_fmt_Error_Debug_fmt is fmt::Error's derived Debug, a unit struct's: its name. */
static bool
std_fmt_Error_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_write_str(f, "Error", 5);
}

/* STD_FMT_MOD_FILE is core's fmt/mod.rs, where str's Debug slices the str it writes. */
#define STD_FMT_MOD_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "fmt" STD_SEP "mod.rs"
#define STD_FMT_NUM_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "fmt" STD_SEP "num.rs"

/* std_fmt_string_write_str is String's fmt::Write::write_str, a push_str. */
static bool
std_fmt_string_write_str(void *self, const char *s, size_t len)
{
	std_string_push_str_Unchecked((StdString *) self, s, len);
	return true;
}

/* std_fmt_encode_utf8 writes a char's UTF-8 bytes and returns how many it wrote. */
size_t
std_fmt_encode_utf8(uint32 c, char *buf)
{
	if (c < 0x80)
	{
		buf[0] = (char) c;
		return 1;
	}
	if (c < 0x800)
	{
		buf[0] = (char) (0xC0 | (c >> 6));
		buf[1] = (char) (0x80 | (c & 0x3F));
		return 2;
	}
	if (c < 0x10000)
	{
		buf[0] = (char) (0xE0 | (c >> 12));
		buf[1] = (char) (0x80 | ((c >> 6) & 0x3F));
		buf[2] = (char) (0x80 | (c & 0x3F));
		return 3;
	}
	buf[0] = (char) (0xF0 | (c >> 18));
	buf[1] = (char) (0x80 | ((c >> 12) & 0x3F));
	buf[2] = (char) (0x80 | ((c >> 6) & 0x3F));
	buf[3] = (char) (0x80 | (c & 0x3F));
	return 4;
}

/*
 * std_fmt_Write_write_char is fmt::Write's default write_char, for a
 * writer that has only write_str: the char's UTF-8 in a four-byte buffer,
 * written.
 */
bool
std_fmt_Write_write_char(bool (*write_str) (void *self, const char *s, size_t len), void *self, uint32 c)
{
	char		buf[4] = {0};
	size_t		len = std_fmt_encode_utf8(c, buf);

	return write_str(self, buf, len);
}

/* std_fmt_string_write_char is String's fmt::Write::write_char, a push. */
static bool
std_fmt_string_write_char(void *self, uint32 c)
{
	std_string_push_Unchecked((StdString *) self, c);
	return true;
}

/* STD_STRING_WRITE_OPS is String's table of fmt::Write. */
const StdFmtWriteOps STD_STRING_WRITE_OPS = {std_fmt_string_write_str, std_fmt_string_write_char};

/* std_fmt_write_str is Formatter::write_str, to the formatter's buffer. */
bool
std_fmt_write_str(StdFormatter *f, const char *s, size_t len)
{
	return f->ops->write_str(f->buf, s, len);
}

/* std_fmt_write_char is the formatter's buffer's write_char. */
bool
std_fmt_write_char(StdFormatter *f, uint32 c)
{
	return f->ops->write_char(f->buf, c);
}

/* std_fmt_as_str_Unchecked is Arguments::as_str as std's own code runs it: the one literal of a template of one, or none. */
bool
std_fmt_as_str_Unchecked(const StdFmtArguments *args, const char **s, size_t *len)
{
	if (args->pieces_len == 0)
	{
		*s = "";
		*len = 0;
		return true;
	}
	if (args->pieces_len == 1 && args->pieces[0].literal != NULL)
	{
		*s = args->pieces[0].literal;
		*len = args->pieces[0].literal_len;
		return true;
	}
	return false;
}

/*
 * std_fmt_as_str_Checked is Arguments::as_str as the crates' code runs it:
 * the literal's slice made by slice::from_raw_parts, checked.
 */
static bool
std_fmt_as_str_Checked(const StdFmtArguments *args, const char **s, size_t *len)
{
	if (!std_fmt_as_str_Unchecked(args, s, len))
		return false;
	std_slice_from_raw_parts_check(*s, *len, 1, 1, STD_FMT_MOD_FILE, 881, 42);
	return true;
}

/*
 * std_fmt_write is fmt::write: each literal piece written as it is, and each
 * placeholder's argument formatted by a new formatter with its options.
 */
bool
std_fmt_write(const StdFmtWriteOps *ops, void *output,
			  const StdFmtArguments *args)
{
	const char *s;
	size_t		len;

	if (std_fmt_as_str_Unchecked(args, &s, &len))
		return ops->write_str(output, s, len);
	for (size_t i = 0; i < args->pieces_len; i++)
	{
		const StdFmtPiece *piece = &args->pieces[i];

		if (piece->literal != NULL)
		{
			if (!ops->write_str(output, piece->literal, piece->literal_len))
				return false;
		}
		else
		{
			StdFormatter f = {ops, output, piece->spec};
			const StdFmtArg *arg = &args->args[piece->arg];

			if (!arg->fmt(arg->value, &f))
				return false;
		}
	}
	return true;
}

/*
 * std_fmt_write_fmt_Checked is Formatter::write_fmt, write! into a
 * formatter, as the crates' code runs it: a template of one literal
 * written as it is, else fmt::write.
 */
bool
std_fmt_write_fmt_Checked(StdFormatter *f, const StdFmtArguments *args)
{
	const char *s;
	size_t		len;

	if (std_fmt_as_str_Checked(args, &s, &len))
		return f->ops->write_str(f->buf, s, len);
	return std_fmt_write(f->ops, f->buf, args);
}

/* std_fmt_write_fmt_Unchecked is Formatter::write_fmt as std's own code runs it. */
bool
std_fmt_write_fmt_Unchecked(StdFormatter *f, const StdFmtArguments *args)
{
	const char *s;
	size_t		len;

	if (std_fmt_as_str_Unchecked(args, &s, &len))
		return f->ops->write_str(f->buf, s, len);
	return std_fmt_write(f->ops, f->buf, args);
}

/*
 * std_fmt_estimated_capacity is Arguments::estimated_capacity: the literal pieces'
 * length, doubled when the template has a placeholder, or nothing when it
 * starts with one and its literals are short.
 */
static size_t
std_fmt_estimated_capacity(const StdFmtArguments *args)
{
	size_t		length = 0;
	bool		starts_with_placeholder = false;
	const char *s;
	size_t		len;

	if (std_fmt_as_str_Unchecked(args, &s, &len))
		return len;
	for (size_t i = 0; i < args->pieces_len; i++)
	{
		if (args->pieces[i].literal != NULL)
			length += args->pieces[i].literal_len;
		else if (length == 0)
			starts_with_placeholder = true;
	}
	if (starts_with_placeholder && length < 16)
		return 0;
	else
		return length * 2;
}

/* StdFmtFormatFrame is format_inner's objects: the String it writes, which it frees while it unwinds. */
typedef struct StdFmtFormatFrame
{
	StdString  *result;
	const StdFmtArguments *args;
} StdFmtFormatFrame;

/* std_fmt_format_inner_body writes the arguments into the frame's String. */
static void
std_fmt_format_inner_body(void *frame_)
{
	StdFmtFormatFrame *frame = frame_;

	if (!std_fmt_write(&STD_STRING_WRITE_OPS, frame->result, frame->args))
	{
		static const char expected[] = "a formatting trait implementation returned an error when the underlying stream did not";
		char		error = 0;

		std_result_unwrap_failed(expected, sizeof(expected) - 1, &error, std_fmt_Error_Debug_fmt, FMT_FILE, 657, 14);
	}
}

/* std_fmt_format_inner_pad frees the String when a write or the expect unwinds. */
static void
std_fmt_format_inner_pad(void *frame_)
{
	StdFmtFormatFrame *frame = frame_;

	std_vec_free_Unchecked(frame->result, 1, 1);
}

/*
 * std_fmt_format_inner is alloc::fmt::format's format_inner: a String of the
 * estimated capacity written to.
 */
static void
std_fmt_format_inner(StdString *result, const StdFmtArguments *args)
{
	StdFmtFormatFrame frame = {result, args};

	std_vec_with_capacity_Unchecked(result, std_fmt_estimated_capacity(args), 1, 1);
	std_landing(&frame, std_fmt_format_inner_body, std_fmt_format_inner_pad);
}

/*
 * std_fmt_format_Checked is alloc::fmt::format as the crates' code runs it: a
 * template of one literal made into a String as it is, else format_inner.
 */
void
std_fmt_format_Checked(StdString *result, const StdFmtArguments *args)
{
	const char *s;
	size_t		len;

	if (std_fmt_as_str_Checked(args, &s, &len))
		std_string_from_Checked(result, s, len);
	else
		std_fmt_format_inner(result, args);
}

/* std_fmt_format_Unchecked is alloc::fmt::format as std's own code runs it. */
void
std_fmt_format_Unchecked(StdString *result, const StdFmtArguments *args)
{
	const char *s;
	size_t		len;

	if (std_fmt_as_str_Unchecked(args, &s, &len))
		std_string_from_Unchecked(result, s, len);
	else
		std_fmt_format_inner(result, args);
}

/* STRING_FILE is alloc/src/string.rs as Rust's panics place it. */
#define STRING_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "string.rs"

/*
 * std_to_string is ToString's default to_string: a new String, empty, that a
 * formatter with default options writes the value's Display to.
 */
void
std_to_string(const void *value, StdFmtFn display, StdString *result)
{
	StdFormatter f = {&STD_STRING_WRITE_OPS, result, STD_FMT_DEFAULT};

	std_vec_new(result, 1, 1);
	if (!display(value, &f))
	{
		static const char expected[] = "a Display implementation returned an error unexpectedly";
		char		error = 0;

		std_result_unwrap_failed(expected, sizeof(expected) - 1, &error, std_fmt_Error_Debug_fmt, STRING_FILE, 2953, 14);
	}
}

/*
 * std_fmt_padding is Formatter::padding: the fill written before the padded
 * text by the alignment, or the default's, returning the PostPadding of the
 * fill it read and what is left for after it.
 */
bool
std_fmt_padding(StdFormatter *f, uint16 pad, StdFmtAlign defaulted, uint32 *post_fill, uint16 *post_padding)
{
	StdFmtAlign align = f->options.align == STD_FMT_ALIGN_UNKNOWN ? defaulted : f->options.align;
	uint32		fill = f->options.fill;
	uint16		padding_left;

	switch (align)
	{
		case STD_FMT_ALIGN_LEFT:
			padding_left = 0;
			break;
		case STD_FMT_ALIGN_RIGHT:
			padding_left = pad;
			break;
		default:
			padding_left = pad / 2;
			break;
	}
	for (uint16 i = 0; i < padding_left; i++)
		if (!std_fmt_write_char(f, fill))
			return false;
	*post_fill = fill;
	*post_padding = (uint16) (pad - padding_left);
	return true;
}

/* std_fmt_post_padding is PostPadding::write: the fill written after the padded text. */
bool
std_fmt_post_padding(StdFormatter *f, uint32 fill, uint16 pad)
{
	for (uint16 i = 0; i < pad; i++)
		if (!std_fmt_write_char(f, fill))
			return false;
	return true;
}

static const char *std_fmt_slice_buffer_to_str(const char *buf, size_t len, size_t offset,
											   size_t *result_len);

/* std_fmt_pad_integral_write_prefix is pad_integral's write_prefix: the sign if there is one, then the prefix if requested. */
static bool
std_fmt_pad_integral_write_prefix(StdFormatter *f, uint32 sign, const char *prefix)
{
	if (sign != 0 && !std_fmt_write_char(f, sign))
		return false;
	if (prefix != NULL)
		return std_fmt_write_str(f, prefix, strlen(prefix));
	return true;
}

/*
 * std_fmt_pad_integral is Formatter::pad_integral: an integer's digits with
 * its sign, its prefix under {:#}, and the padding its width asks for.
 */
bool
std_fmt_pad_integral(StdFormatter *f, bool is_nonnegative, const char *prefix,
					 const char *buf, size_t len)
{
	size_t		width = len;
	uint32		sign = 0;
	const char *shown = NULL;
	uint16		min;
	uint32		post_fill;
	uint16		post;

	if (!is_nonnegative)
	{
		sign = '-';
		width += 1;
	}
	else if (f->options.sign_plus)
	{
		sign = '+';
		width += 1;
	}
	if (f->options.alternate)
	{
		width += std_str_Chars_count_Unchecked(prefix, strlen(prefix));
		shown = prefix;
	}
	min = f->options.width;
	if (width >= min)
	{
		if (!std_fmt_pad_integral_write_prefix(f, sign, shown))
			return false;
		return std_fmt_write_str(f, buf, len);
	}
	else if (f->options.sign_aware_zero_pad)
	{
		StdFmtSpec	old_options = f->options;

		f->options.fill = '0';
		f->options.align = STD_FMT_ALIGN_RIGHT;
		if (!std_fmt_pad_integral_write_prefix(f, sign, shown))
			return false;
		if (!std_fmt_padding(f, (uint16) (min - width), STD_FMT_ALIGN_RIGHT, &post_fill, &post))
			return false;
		if (!std_fmt_write_str(f, buf, len))
			return false;
		if (!std_fmt_post_padding(f, post_fill, post))
			return false;
		f->options = old_options;
		return true;
	}
	else
	{
		if (!std_fmt_padding(f, (uint16) (min - width), STD_FMT_ALIGN_RIGHT, &post_fill, &post))
			return false;
		if (!std_fmt_pad_integral_write_prefix(f, sign, shown))
			return false;
		if (!std_fmt_write_str(f, buf, len))
			return false;
		return std_fmt_post_padding(f, post_fill, post);
	}
}

/*
 * std_fmt_pad is Formatter::pad: a string cut to its precision in chars and
 * padded to its width, written as it is when the options ask for neither.
 */
bool
std_fmt_pad(StdFormatter *f, const char *s, size_t len)
{
	size_t		count;
	uint32		post_fill;
	uint16		post;

	if (!f->options.has_width && !f->options.has_precision)
		return std_fmt_write_str(f, s, len);
	if (f->options.has_precision)
	{
		size_t		max_char_count = f->options.precision;
		StdChars	iter = std_str_chars(s, len);
		size_t		front_offset = 0;
		size_t		remaining = 0;

		for (size_t i = 0; i < max_char_count; i++)
		{
			size_t		pre_len = (size_t) (iter.iter.end - iter.iter.ptr);
			uint32		ch;

			if (!std_str_Chars_next_Unchecked(&iter, &ch))
			{
				remaining = max_char_count - i;
				break;
			}
			front_offset += pre_len - std_ptr_offset_from_unsigned_Unchecked(iter.iter.end, iter.iter.ptr, 1,
																			 STD_PTR_NON_NULL_FILE, 898, 32);
		}
		len = front_offset;
		count = max_char_count - remaining;
	}
	else
		count = std_str_Chars_count_Unchecked(s, len);
	if (count < f->options.width)
	{
		if (!std_fmt_padding(f, (uint16) (f->options.width - count), STD_FMT_ALIGN_LEFT, &post_fill, &post))
			return false;
		if (!std_fmt_write_str(f, s, len))
			return false;
		return std_fmt_post_padding(f, post_fill, post);
	}
	return std_fmt_write_str(f, s, len);
}

/*
 * std_fmt_pad_formatted_parts is Formatter::pad_formatted_parts: a number's
 * formatted parts padded to the width, its sign first under the
 * sign-aware zero padding.
 */
bool
std_fmt_pad_formatted_parts(StdFormatter *f, const StdNumfmtFormatted *formatted)
{
	if (f->options.width == 0)
		return std_fmt_write_formatted_parts(f, formatted);
	else
	{
		StdNumfmtFormatted formatted_2 = *formatted;
		uint16		width = f->options.width;
		StdFmtSpec	old_options = f->options;
		size_t		len;
		bool		ret;

		if (f->options.sign_aware_zero_pad)
		{
			StdBytes	sign = formatted_2.sign;

			if (!std_fmt_write_str(f, (const char *) sign.ptr, sign.len))
				return false;
			formatted_2.sign.len = 0;
			width = std_saturating_sub_u16(width, (uint16) sign.len);
			f->options.fill = '0';
			f->options.align = STD_FMT_ALIGN_RIGHT;
		}
		len = std_numfmt_Formatted_len(&formatted_2);
		if ((size_t) width <= len)
			ret = std_fmt_write_formatted_parts(f, &formatted_2);
		else
		{
			uint16		len_2;
			uint32		post_fill;
			uint16		post_padding;

			if (len > PG_UINT16_MAX)
			{
				static const char text[] = "internal error: entered unreachable code";

				std_panic(STD_FMT_MOD_FILE, 2014, 31, text, sizeof(text) - 1);
			}
			len_2 = (uint16) len;
			if (!std_fmt_padding(f, (uint16) (width - len_2), STD_FMT_ALIGN_RIGHT, &post_fill, &post_padding))
				return false;
			if (!std_fmt_write_formatted_parts(f, &formatted_2))
				return false;
			ret = std_fmt_post_padding(f, post_fill, post_padding);
		}
		f->options = old_options;
		return ret;
	}
}

/* std_fmt_write_formatted_parts_write_bytes is write_formatted_parts' write_bytes: bytes written as the str they are. */
static bool
std_fmt_write_formatted_parts_write_bytes(StdFormatter *f, const uint8 *s, size_t len)
{
	return std_fmt_write_str(f, (const char *) s, len);
}

/* std_fmt_write_formatted_parts is Formatter::write_formatted_parts: a number's sign and parts, written. */
bool
std_fmt_write_formatted_parts(StdFormatter *f, const StdNumfmtFormatted *formatted)
{
	if (formatted->sign.len != 0 && !std_fmt_write_str(f, (const char *) formatted->sign.ptr, formatted->sign.len))
		return false;
	for (size_t i = 0; i < formatted->parts_len; i++)
	{
		const StdNumfmtPart *part = &formatted->parts[i];

		switch (part->kind)
		{
			case STD_NUMFMT_PART_ZERO:
				{
					static const char ZEROES[] = "0000000000000000000000000000000000000000000000000000000000000000";
					size_t		nzeroes = part->u.zero;

					while (nzeroes > sizeof(ZEROES) - 1)
					{
						if (!std_fmt_write_str(f, ZEROES, sizeof(ZEROES) - 1))
							return false;
						nzeroes -= sizeof(ZEROES) - 1;
					}
					if (nzeroes > 0)
					{
						std_str_index_range(ZEROES, sizeof(ZEROES) - 1, 0, nzeroes, STD_FMT_MOD_FILE, 2053, 51);
						if (!std_fmt_write_str(f, ZEROES, nzeroes))
							return false;
					}
				}
				break;
			case STD_NUMFMT_PART_NUM:
				{
					uint16		v = part->u.num;
					uint8		s[5] = {0};
					size_t		len = std_numfmt_Part_len(part);

					std_range_to_check(len, lengthof(s), STD_FMT_MOD_FILE, 2059, 31);
					for (size_t j = len; j > 0; j--)
					{
						s[j - 1] = (uint8) ('0' + (uint8) (v % 10));
						v /= 10;
					}
					if (!std_fmt_write_formatted_parts_write_bytes(f, s, len))
						return false;
				}
				break;
			case STD_NUMFMT_PART_COPY:
				if (!std_fmt_write_formatted_parts_write_bytes(f, part->u.copy.ptr, part->u.copy.len))
					return false;
				break;
		}
	}
	return true;
}

/* STD_FMT_LOWER_HEX_DIG_TAB and STD_FMT_UPPER_HEX_DIG_TAB are LowerHex's and UpperHex's digit tables. */
#define STD_FMT_LOWER_HEX_DIG_TAB "0123456789abcdef"
#define STD_FMT_UPPER_HEX_DIG_TAB "0123456789ABCDEF"

/*
 * std_fmt_radix_integer_hex_fmt is radix_integer!'s unsigned fmt for
 * LowerHex and UpperHex: an unsigned integer's hex digits from a table,
 * least significant first, right-aligned in a buffer of the type's most
 * digits, through pad_integral with the prefix 0x. Its index checks report
 * the line and column of the type's radix_integers! invocation, as rustc
 * places a check inside a macro's expansion.
 */
static bool
std_fmt_radix_integer_hex_fmt(uint64 self, const char *dig_tab, size_t max_dig_n, StdFormatter *f,
							  const char *file, uint32 line, uint32 col)
{
	char		buf[16];
	size_t		offset = max_dig_n;
	uint64		remain = self;
	const char *digits;
	size_t		digits_len;

	for (;;)
	{
		uint64		digit = remain % 16;

		remain /= 16;
		offset -= 1;
		std_index_check(offset, max_dig_n, file, line, col);
		std_index_check((size_t) digit, 16, file, line, col);
		buf[offset] = dig_tab[digit];
		if (remain == 0)
			break;
	}
	digits = std_fmt_slice_buffer_to_str(buf, max_dig_n, offset, &digits_len);
	return std_fmt_pad_integral(f, true, "0x", digits, digits_len);
}

/* std_usize_LowerHex_fmt is usize's LowerHex, radix_integer!'s at usize. */
static bool
std_usize_LowerHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const size_t *) value, STD_FMT_LOWER_HEX_DIG_TAB, 16, f, STD_FMT_NUM_FILE, 72, 1);
}

/* std_usize_UpperHex_fmt is usize's UpperHex, radix_integer!'s at usize. */
static bool
std_usize_UpperHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const size_t *) value, STD_FMT_UPPER_HEX_DIG_TAB, 16, f, STD_FMT_NUM_FILE, 72, 1);
}

/*
 * std_fmt_pointer_fmt_inner is pointer_fmt_inner: an address as usize's
 * LowerHex with its 0x, zero-padded to a usize's digits and its prefix under
 * the alternate flag, the formatter's options restored after.
 */
static bool
std_fmt_pointer_fmt_inner(size_t ptr_addr, StdFormatter *f)
{
	StdFmtSpec	old_options = f->options;
	bool		ret;

	if (f->options.alternate)
	{
		f->options.sign_aware_zero_pad = true;
		if (!f->options.has_width)
		{
			f->options.has_width = true;
			f->options.width = (uint16) (sizeof(size_t) * 8 / 4 + 2);
		}
	}
	f->options.alternate = true;
	ret = std_usize_LowerHex_fmt(&ptr_addr, f);
	f->options = old_options;
	return ret;
}

/* std_ptr_Debug_fmt is a raw pointer's Debug, Pointer's fmt of its address; value points to the pointer. */
bool
std_ptr_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_pointer_fmt_inner((size_t) (uintptr_t) *(const void *const *) value, f);
}

/* std_u8_LowerHex_fmt is u8's LowerHex, radix_integer!'s at u8. */
bool
std_u8_LowerHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const uint8 *) value, STD_FMT_LOWER_HEX_DIG_TAB, 2, f, STD_FMT_NUM_FILE, 73, 1);
}

/* std_u8_UpperHex_fmt is u8's UpperHex, radix_integer!'s at u8. */
bool
std_u8_UpperHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const uint8 *) value, STD_FMT_UPPER_HEX_DIG_TAB, 2, f, STD_FMT_NUM_FILE, 73, 1);
}

/* std_u16_LowerHex_fmt is u16's LowerHex, radix_integer!'s at u16. */
bool
std_u16_LowerHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const uint16 *) value, STD_FMT_LOWER_HEX_DIG_TAB, 4, f, STD_FMT_NUM_FILE, 74, 1);
}

/* std_u16_UpperHex_fmt is u16's UpperHex, radix_integer!'s at u16. */
static bool
std_u16_UpperHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const uint16 *) value, STD_FMT_UPPER_HEX_DIG_TAB, 4, f, STD_FMT_NUM_FILE, 74, 1);
}

/* std_u32_LowerHex_fmt is u32's LowerHex, radix_integer!'s at u32. */
bool
std_u32_LowerHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const uint32 *) value, STD_FMT_LOWER_HEX_DIG_TAB, 8, f, STD_FMT_NUM_FILE, 75, 1);
}

/* std_u32_UpperHex_fmt is u32's UpperHex, radix_integer!'s at u32. */
bool
std_u32_UpperHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const uint32 *) value, STD_FMT_UPPER_HEX_DIG_TAB, 8, f, STD_FMT_NUM_FILE, 75, 1);
}

/* std_i16_LowerHex_fmt is i16's LowerHex, its two's complement as u16's. */
static bool
std_i16_LowerHex_fmt(const void *value, StdFormatter *f)
{
	uint16		tmp_1 = (uint16) *(const int16 *) value;

	return std_u16_LowerHex_fmt(&tmp_1, f);
}

/* std_i16_UpperHex_fmt is i16's UpperHex, its two's complement as u16's. */
static bool
std_i16_UpperHex_fmt(const void *value, StdFormatter *f)
{
	uint16		tmp_1 = (uint16) *(const int16 *) value;

	return std_u16_UpperHex_fmt(&tmp_1, f);
}

/* std_i32_LowerHex_fmt is i32's LowerHex, its two's complement as u32's. */
static bool
std_i32_LowerHex_fmt(const void *value, StdFormatter *f)
{
	uint32		tmp_1 = (uint32) *(const int32 *) value;

	return std_u32_LowerHex_fmt(&tmp_1, f);
}

/* std_i32_UpperHex_fmt is i32's UpperHex, its two's complement as u32's. */
static bool
std_i32_UpperHex_fmt(const void *value, StdFormatter *f)
{
	uint32		tmp_1 = (uint32) *(const int32 *) value;

	return std_u32_UpperHex_fmt(&tmp_1, f);
}

/* std_u64_LowerHex_fmt is u64's LowerHex, radix_integer!'s at u64. */
bool
std_u64_LowerHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const uint64 *) value, STD_FMT_LOWER_HEX_DIG_TAB, 16, f, STD_FMT_NUM_FILE, 76, 1);
}

/* std_u64_UpperHex_fmt is u64's UpperHex, radix_integer!'s at u64. */
bool
std_u64_UpperHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt(*(const uint64 *) value, STD_FMT_UPPER_HEX_DIG_TAB, 16, f, STD_FMT_NUM_FILE, 76, 1);
}

/* std_i64_LowerHex_fmt is i64's LowerHex, its two's complement as u64's. */
static bool
std_i64_LowerHex_fmt(const void *value, StdFormatter *f)
{
	uint64		tmp_1 = (uint64) *(const int64 *) value;

	return std_u64_LowerHex_fmt(&tmp_1, f);
}

/* std_i64_UpperHex_fmt is i64's UpperHex, its two's complement as u64's. */
static bool
std_i64_UpperHex_fmt(const void *value, StdFormatter *f)
{
	uint64		tmp_1 = (uint64) *(const int64 *) value;

	return std_u64_UpperHex_fmt(&tmp_1, f);
}

/* STD_FMT_EXPECT_999 and STD_FMT_EXPECT_1E4 are _fmt_inner's two expect messages. */
#define STD_FMT_EXPECT_999 "branch is not hit for types that cannot fit 999 (u8)"
#define STD_FMT_EXPECT_1E4 "branch is not hit for types that cannot fit 1E4 (u8)"

/* std_fmt_TryFromIntError_Debug_fmt is TryFromIntError's derived Debug: the tuple struct of its (). */
bool
std_fmt_TryFromIntError_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple tuple = std_fmt_debug_tuple(f, "TryFromIntError", sizeof("TryFromIntError") - 1);

	std_DebugTuple_field(&tuple, value, std_unit_Debug_fmt);
	return std_DebugTuple_finish(&tuple);
}

/*
 * std_fmt_try_into_expect is _fmt_inner's `<i32 literal>.try_into().expect(..)`
 * into an unsigned type whose most value is max: the value, or Result::expect's
 * panic with the TryFromIntError at its location when it doesn't fit.
 */
static uint64
std_fmt_try_into_expect(int32 value, uint64 max, const char *msg, size_t msg_len, const char *file,
						uint32 line, uint32 col)
{
	if (value < 0 || (uint64) value > max)
	{
		char		error = 0;

		std_result_unwrap_failed(msg, msg_len, &error, std_fmt_TryFromIntError_Debug_fmt, file, line, col);
	}
	return (uint64) value;
}

/* STD_FMT_DECIMAL_PAIRS is core's DECIMAL_PAIRS: the hundred two-digit pairs an integer's Display writes from. */
static const char STD_FMT_DECIMAL_PAIRS[200] =
"0001020304050607080910111213141516171819"
"2021222324252627282930313233343536373839"
"4041424344454647484950515253545556575859"
"6061626364656667686970717273747576777879"
"8081828384858687888990919293949596979899";

/* std_fmt_slice_buffer_to_str is slice_buffer_to_str: the written tail of an integer's buffer, from offset. */
static const char *
std_fmt_slice_buffer_to_str(const char *buf, size_t len, size_t offset, size_t *result_len)
{
	*result_len = len - offset;
	return buf + offset;
}

/*
 * std_fmt_u8_fmt_inner is u8::_fmt_inner: its decimal digits written to
 * the end of the buffer, four at a time, then a pair, then one, through
 * DECIMAL_PAIRS, whose four-digit loop size_of::<u8>() > 1 rules out, returning where they start.
 */
static size_t
std_fmt_u8_fmt_inner(uint8 self, char *buf, size_t len)
{
	size_t		offset = len;
	uint8		remain = self;

	if (remain > 9)
	{
		size_t		pair;

		offset -= 2;
		pair = (size_t) (remain % 100);
		remain /= 100;
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 1];
	}
	if (remain != 0 || self == 0)
	{
		size_t		last;

		offset -= 1;
		last = (size_t) (remain & 15);
		std_index_check(offset, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(last * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset] = STD_FMT_DECIMAL_PAIRS[last * 2 + 1];
	}
	return offset;
}

/* std_fmt_u8_fmt is u8::_fmt: its decimal digits, the written tail of the buffer. */
static const char *
std_fmt_u8_fmt(uint8 self, char *buf, size_t len, size_t *result_len)
{
	size_t		offset = std_fmt_u8_fmt_inner(self, buf, len);

	return std_fmt_slice_buffer_to_str(buf, len, offset, result_len);
}

/* std_u8_Display_fmt is u8's Display: its digits, nonnegative, through pad_integral. */
bool
std_u8_Display_fmt(const void *value, StdFormatter *f)
{
	char		buf[3];
	size_t		digits_len;
	const char *digits = std_fmt_u8_fmt(*(const uint8 *) value, buf, sizeof(buf), &digits_len);

	return std_fmt_pad_integral(f, true, "", digits, digits_len);
}

/*
 * std_fmt_u16_fmt_inner is u16::_fmt_inner: its decimal digits written to
 * the end of the buffer, four at a time, then a pair, then one, through
 * DECIMAL_PAIRS, returning where they start.
 */
static size_t
std_fmt_u16_fmt_inner(uint16 self, char *buf, size_t len)
{
	size_t		offset = len;
	uint16		remain = self;

	while (sizeof(uint16) > 1 &&
		   remain > (uint16) std_fmt_try_into_expect(999, PG_UINT16_MAX, STD_FMT_EXPECT_999,
													 sizeof(STD_FMT_EXPECT_999) - 1, STD_FMT_NUM_FILE, 598, 5))
	{
		uint16		scale;
		uint16		quad;
		size_t		pair1;
		size_t		pair2;

		offset -= 4;
		scale = (uint16) std_fmt_try_into_expect(10000, PG_UINT16_MAX, STD_FMT_EXPECT_1E4, sizeof(STD_FMT_EXPECT_1E4) - 1, STD_FMT_NUM_FILE, 598, 5);
		quad = std_rem_u16(remain, scale, STD_FMT_NUM_FILE, 598, 5);
		remain = std_div_u16(remain, scale, STD_FMT_NUM_FILE, 598, 5);
		pair1 = (size_t) (quad / 100);
		pair2 = (size_t) (quad % 100);
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair1 * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair1 * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair1 * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair1 * 2 + 1];
		std_index_check(offset + 2, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair2 * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 2] = STD_FMT_DECIMAL_PAIRS[pair2 * 2 + 0];
		std_index_check(offset + 3, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair2 * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 3] = STD_FMT_DECIMAL_PAIRS[pair2 * 2 + 1];
	}
	if (remain > 9)
	{
		size_t		pair;

		offset -= 2;
		pair = (size_t) (remain % 100);
		remain /= 100;
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 1];
	}
	if (remain != 0 || self == 0)
	{
		size_t		last;

		offset -= 1;
		last = (size_t) (remain & 15);
		std_index_check(offset, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(last * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset] = STD_FMT_DECIMAL_PAIRS[last * 2 + 1];
	}
	return offset;
}

/* std_fmt_u16_fmt is u16::_fmt: its decimal digits, the written tail of the buffer. */
static const char *
std_fmt_u16_fmt(uint16 self, char *buf, size_t len, size_t *result_len)
{
	size_t		offset = std_fmt_u16_fmt_inner(self, buf, len);

	return std_fmt_slice_buffer_to_str(buf, len, offset, result_len);
}

/* std_u16_Display_fmt is u16's Display: its digits, nonnegative, through pad_integral. */
bool
std_u16_Display_fmt(const void *value, StdFormatter *f)
{
	char		buf[5];
	size_t		digits_len;
	const char *digits = std_fmt_u16_fmt(*(const uint16 *) value, buf, sizeof(buf), &digits_len);

	return std_fmt_pad_integral(f, true, "", digits, digits_len);
}

/*
 * std_fmt_u32_fmt_inner is u32::_fmt_inner: its decimal digits written to
 * the end of the buffer, four at a time, then a pair, then one, through
 * DECIMAL_PAIRS, returning where they start.
 */
static size_t
std_fmt_u32_fmt_inner(uint32 self, char *buf, size_t len)
{
	size_t		offset = len;
	uint32		remain = self;

	while (sizeof(uint32) > 1 &&
		   remain > (uint32) std_fmt_try_into_expect(999, PG_UINT32_MAX, STD_FMT_EXPECT_999,
													 sizeof(STD_FMT_EXPECT_999) - 1, STD_FMT_NUM_FILE, 598, 5))
	{
		uint32		scale;
		uint32		quad;
		size_t		pair1;
		size_t		pair2;

		offset -= 4;
		scale = (uint32) std_fmt_try_into_expect(10000, PG_UINT32_MAX, STD_FMT_EXPECT_1E4, sizeof(STD_FMT_EXPECT_1E4) - 1, STD_FMT_NUM_FILE, 598, 5);
		quad = std_rem_u32(remain, scale, STD_FMT_NUM_FILE, 598, 5);
		remain = std_div_u32(remain, scale, STD_FMT_NUM_FILE, 598, 5);
		pair1 = (size_t) (quad / 100);
		pair2 = (size_t) (quad % 100);
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair1 * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair1 * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair1 * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair1 * 2 + 1];
		std_index_check(offset + 2, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair2 * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 2] = STD_FMT_DECIMAL_PAIRS[pair2 * 2 + 0];
		std_index_check(offset + 3, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair2 * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 3] = STD_FMT_DECIMAL_PAIRS[pair2 * 2 + 1];
	}
	if (remain > 9)
	{
		size_t		pair;

		offset -= 2;
		pair = (size_t) (remain % 100);
		remain /= 100;
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 1];
	}
	if (remain != 0 || self == 0)
	{
		size_t		last;

		offset -= 1;
		last = (size_t) (remain & 15);
		std_index_check(offset, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(last * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset] = STD_FMT_DECIMAL_PAIRS[last * 2 + 1];
	}
	return offset;
}

/* std_fmt_u32_fmt is u32::_fmt: its decimal digits, the written tail of the buffer. */
static const char *
std_fmt_u32_fmt(uint32 self, char *buf, size_t len, size_t *result_len)
{
	size_t		offset = std_fmt_u32_fmt_inner(self, buf, len);

	return std_fmt_slice_buffer_to_str(buf, len, offset, result_len);
}

/* std_u32_Display_fmt is u32's Display: its digits, nonnegative, through pad_integral. */
bool
std_u32_Display_fmt(const void *value, StdFormatter *f)
{
	char		buf[10];
	size_t		digits_len;
	const char *digits = std_fmt_u32_fmt(*(const uint32 *) value, buf, sizeof(buf), &digits_len);

	return std_fmt_pad_integral(f, true, "", digits, digits_len);
}

/*
 * std_u32_to_string is u32's SpecToString, as the crates' code runs it: its
 * digits by _fmt into a buffer of u32::MAX's length, made a String by str's,
 * String::from.
 */
void
std_u32_to_string(uint32 self, StdString *result)
{
	char		buf[10];
	size_t		digits_len;
	const char *digits = std_fmt_u32_fmt(self, buf, sizeof(buf), &digits_len);

	std_string_from_Checked(result, digits, digits_len);
}

/*
 * std_fmt_u64_fmt_inner is u64::_fmt_inner: its decimal digits written to
 * the end of the buffer, four at a time, then a pair, then one, through
 * DECIMAL_PAIRS, returning where they start.
 */
static size_t
std_fmt_u64_fmt_inner(uint64 self, char *buf, size_t len)
{
	size_t		offset = len;
	uint64		remain = self;

	while (sizeof(uint64) > 1 &&
		   remain > (uint64) std_fmt_try_into_expect(999, PG_UINT64_MAX, STD_FMT_EXPECT_999,
													 sizeof(STD_FMT_EXPECT_999) - 1, STD_FMT_NUM_FILE, 598, 5))
	{
		uint64		scale;
		uint64		quad;
		size_t		pair1;
		size_t		pair2;

		offset -= 4;
		scale = (uint64) std_fmt_try_into_expect(10000, PG_UINT64_MAX, STD_FMT_EXPECT_1E4, sizeof(STD_FMT_EXPECT_1E4) - 1, STD_FMT_NUM_FILE, 598, 5);
		quad = std_rem_u64(remain, scale, STD_FMT_NUM_FILE, 598, 5);
		remain = std_div_u64(remain, scale, STD_FMT_NUM_FILE, 598, 5);
		pair1 = (size_t) (quad / 100);
		pair2 = (size_t) (quad % 100);
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair1 * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair1 * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair1 * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair1 * 2 + 1];
		std_index_check(offset + 2, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair2 * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 2] = STD_FMT_DECIMAL_PAIRS[pair2 * 2 + 0];
		std_index_check(offset + 3, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair2 * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 3] = STD_FMT_DECIMAL_PAIRS[pair2 * 2 + 1];
	}
	if (remain > 9)
	{
		size_t		pair;

		offset -= 2;
		pair = (size_t) (remain % 100);
		remain /= 100;
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 1];
	}
	if (remain != 0 || self == 0)
	{
		size_t		last;

		offset -= 1;
		last = (size_t) (remain & 15);
		std_index_check(offset, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(last * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset] = STD_FMT_DECIMAL_PAIRS[last * 2 + 1];
	}
	return offset;
}

/* std_fmt_u64_fmt is u64::_fmt: its decimal digits, the written tail of the buffer. */
static const char *
std_fmt_u64_fmt(uint64 self, char *buf, size_t len, size_t *result_len)
{
	size_t		offset = std_fmt_u64_fmt_inner(self, buf, len);

	return std_fmt_slice_buffer_to_str(buf, len, offset, result_len);
}

/* std_u64_Display_fmt is u64's Display: its digits, nonnegative, through pad_integral. */
bool
std_u64_Display_fmt(const void *value, StdFormatter *f)
{
	char		buf[20];
	size_t		digits_len;
	const char *digits = std_fmt_u64_fmt(*(const uint64 *) value, buf, sizeof(buf), &digits_len);

	return std_fmt_pad_integral(f, true, "", digits, digits_len);
}

/*
 * std_fmt_usize_fmt_inner is usize::_fmt_inner: its decimal digits written to
 * the end of the buffer, four at a time, then a pair, then one, through
 * DECIMAL_PAIRS, returning where they start.
 */
static size_t
std_fmt_usize_fmt_inner(size_t self, char *buf, size_t len)
{
	size_t		offset = len;
	size_t		remain = self;

	while (sizeof(size_t) > 1 &&
		   remain > (size_t) std_fmt_try_into_expect(999, SIZE_MAX, STD_FMT_EXPECT_999,
													 sizeof(STD_FMT_EXPECT_999) - 1, STD_FMT_NUM_FILE, 598, 5))
	{
		size_t		scale;
		size_t		quad;
		size_t		pair1;
		size_t		pair2;

		offset -= 4;
		scale = (size_t) std_fmt_try_into_expect(10000, SIZE_MAX, STD_FMT_EXPECT_1E4, sizeof(STD_FMT_EXPECT_1E4) - 1, STD_FMT_NUM_FILE, 598, 5);
		quad = std_rem_u64(remain, scale, STD_FMT_NUM_FILE, 598, 5);
		remain = std_div_u64(remain, scale, STD_FMT_NUM_FILE, 598, 5);
		pair1 = (size_t) (quad / 100);
		pair2 = (size_t) (quad % 100);
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair1 * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair1 * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair1 * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair1 * 2 + 1];
		std_index_check(offset + 2, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair2 * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 2] = STD_FMT_DECIMAL_PAIRS[pair2 * 2 + 0];
		std_index_check(offset + 3, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair2 * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 3] = STD_FMT_DECIMAL_PAIRS[pair2 * 2 + 1];
	}
	if (remain > 9)
	{
		size_t		pair;

		offset -= 2;
		pair = (size_t) (remain % 100);
		remain /= 100;
		std_index_check(offset + 0, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 0, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 0] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 0];
		std_index_check(offset + 1, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(pair * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset + 1] = STD_FMT_DECIMAL_PAIRS[pair * 2 + 1];
	}
	if (remain != 0 || self == 0)
	{
		size_t		last;

		offset -= 1;
		last = (size_t) (remain & 15);
		std_index_check(offset, len, STD_FMT_NUM_FILE, 598, 5);
		std_index_check(last * 2 + 1, 200, STD_FMT_NUM_FILE, 598, 5);
		buf[offset] = STD_FMT_DECIMAL_PAIRS[last * 2 + 1];
	}
	return offset;
}

/* std_fmt_usize_fmt is usize::_fmt: its decimal digits, the written tail of the buffer. */
static const char *
std_fmt_usize_fmt(size_t self, char *buf, size_t len, size_t *result_len)
{
	size_t		offset = std_fmt_usize_fmt_inner(self, buf, len);

	return std_fmt_slice_buffer_to_str(buf, len, offset, result_len);
}

/* std_usize_Display_fmt is usize's Display: its digits, nonnegative, through pad_integral. */
bool
std_usize_Display_fmt(const void *value, StdFormatter *f)
{
	char		buf[20];
	size_t		digits_len;
	const char *digits = std_fmt_usize_fmt(*(const size_t *) value, buf, sizeof(buf), &digits_len);

	return std_fmt_pad_integral(f, true, "", digits, digits_len);
}

/* std_i16_Display_fmt is i16's Display: its unsigned_abs's digits, with its sign, through pad_integral. */
bool
std_i16_Display_fmt(const void *value, StdFormatter *f)
{
	int16		self = *(const int16 *) value;
	char		buf[5];
	size_t		digits_len;
	const char *digits = std_fmt_u16_fmt(self < 0 ? (uint16) 0 - (uint16) self : (uint16) self, buf, sizeof(buf),
										 &digits_len);

	return std_fmt_pad_integral(f, self >= 0, "", digits, digits_len);
}

/* std_i32_Display_fmt is i32's Display: its unsigned_abs's digits, with its sign, through pad_integral. */
bool
std_i32_Display_fmt(const void *value, StdFormatter *f)
{
	int32		self = *(const int32 *) value;
	char		buf[10];
	size_t		digits_len;
	const char *digits = std_fmt_u32_fmt(self < 0 ? (uint32) 0 - (uint32) self : (uint32) self, buf, sizeof(buf),
										 &digits_len);

	return std_fmt_pad_integral(f, self >= 0, "", digits, digits_len);
}

/* std_i64_Display_fmt is i64's Display: its unsigned_abs's digits, with its sign, through pad_integral. */
bool
std_i64_Display_fmt(const void *value, StdFormatter *f)
{
	int64		self = *(const int64 *) value;
	char		buf[20];
	size_t		digits_len;
	const char *digits = std_fmt_u64_fmt(self < 0 ? (uint64) 0 - (uint64) self : (uint64) self, buf, sizeof(buf),
										 &digits_len);

	return std_fmt_pad_integral(f, self >= 0, "", digits, digits_len);
}

/* std_u8_Debug_fmt is u8's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_u8_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_u8_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_u8_UpperHex_fmt(value, f);
	else
		return std_u8_Display_fmt(value, f);
}

/* std_u16_Debug_fmt is u16's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_u16_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_u16_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_u16_UpperHex_fmt(value, f);
	else
		return std_u16_Display_fmt(value, f);
}

/* std_u32_Debug_fmt is u32's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_u32_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_u32_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_u32_UpperHex_fmt(value, f);
	else
		return std_u32_Display_fmt(value, f);
}

/* STD_FMT_MAYBE_UNINIT_FILE is core's mem/maybe_uninit.rs, where write_copy_of_slice copies. */
#define STD_FMT_MAYBE_UNINIT_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "mem" STD_SEP "maybe_uninit.rs"

/* STD_FMT_U128_MAX_DEC_N is U128_MAX_DEC_N, u128::MAX's 39 decimal digits. */
#define STD_FMT_U128_MAX_DEC_N 39

/*
 * std_fmt_radix_integer_hex_fmt_u128 is radix_integer!'s unsigned fmt for
 * LowerHex and UpperHex at u128: its hex digits from a table, least
 * significant first, right-aligned in a buffer of 32, through pad_integral
 * with the prefix 0x, its index checks at u128's radix_integers! invocation.
 */
static bool
std_fmt_radix_integer_hex_fmt_u128(StdU128 self, const char *dig_tab, StdFormatter *f)
{
	char		buf[32];
	size_t		offset = 32;
	StdU128		remain = self;
	const char *digits;
	size_t		digits_len;

	for (;;)
	{
		size_t		digit = (size_t) (remain.lo & 15);

		remain = std_shr_folded_u128(remain, 4);
		offset = std_wrapping_sub_usize(offset, 1);
		std_index_check(offset, 32, STD_FMT_NUM_FILE, 77, 1);
		std_index_check(digit, 16, STD_FMT_NUM_FILE, 77, 1);
		buf[offset] = dig_tab[digit];
		if (remain.hi == 0 && remain.lo == 0)
			break;
	}
	digits = std_fmt_slice_buffer_to_str(buf, 32, offset, &digits_len);
	return std_fmt_pad_integral(f, true, "0x", digits, digits_len);
}

/* std_u128_LowerHex_fmt is u128's LowerHex, radix_integer!'s at u128. */
static bool
std_u128_LowerHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt_u128(*(const StdU128 *) value, STD_FMT_LOWER_HEX_DIG_TAB, f);
}

/* std_u128_UpperHex_fmt is u128's UpperHex, radix_integer!'s at u128. */
static bool
std_u128_UpperHex_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_radix_integer_hex_fmt_u128(*(const StdU128 *) value, STD_FMT_UPPER_HEX_DIG_TAB, f);
}

/*
 * std_fmt_write_quad is write_quad: a number below 10,000 as four digits
 * into four bytes, through DECIMAL_PAIRS by its high and low pairs, each
 * copied by write_copy_of_slice.
 */
static void
std_fmt_write_quad(char *buf, size_t buf_len, uint64 quad_)
{
	uint32		quad = (uint32) quad_;
	uint32		high = STD_SHR_FOLDED(u32, std_wrapping_mul_u32(quad, (uint32) ((1 << 19) / 100 + 1)), 19);
	uint32		low = std_wrapping_sub_u32(quad, std_wrapping_mul_u32(high, 100));
	size_t		high_at = std_wrapping_mul_usize((size_t) high, 2);
	size_t		low_at = std_wrapping_mul_usize((size_t) low, 2);

	std_range_check(0, 2, buf_len, STD_FMT_NUM_FILE, 845, 8);
	std_range_check(high_at, std_wrapping_add_usize(high_at, 2), 200, STD_FMT_NUM_FILE, 845, 49);
	std_slice_copy_from_slice_Unchecked(buf, 2, STD_FMT_DECIMAL_PAIRS + high_at, 2, 1, 1, STD_FMT_MAYBE_UNINIT_FILE,
										1170, 14);
	std_range_check(2, 4, buf_len, STD_FMT_NUM_FILE, 846, 8);
	std_range_check(low_at, std_wrapping_add_usize(low_at, 2), 200, STD_FMT_NUM_FILE, 846, 49);
	std_slice_copy_from_slice_Unchecked(buf + 2, 2, STD_FMT_DECIMAL_PAIRS + low_at, 2, 1, 1, STD_FMT_MAYBE_UNINIT_FILE,
										1170, 14);
}

/*
 * std_fmt_enc_16lsd is enc_16lsd: a number below 10^16 as sixteen digits at
 * the offset, four at a time from the right.
 */
static void
std_fmt_enc_16lsd(char *buf, size_t offset, uint64 n)
{
	uint64		remain = n;

	for (size_t quad_index = 4; quad_index > 1;)
	{
		uint64		quad;

		quad_index = std_backward_unchecked_usize_Unchecked(quad_index, 1);
		quad = STD_REM_FOLDED(u64, remain, 10000);
		remain = STD_DIV_FOLDED(u64, remain, 10000);
		std_fmt_write_quad(buf + std_wrapping_add_usize(offset, std_wrapping_mul_usize(quad_index, 4)), 4, quad);
	}
	std_fmt_write_quad(buf + offset, 4, remain);
}

/*
 * std_fmt_div_rem_1e16 is div_rem_1e16: n's quotient and remainder by 10^16,
 * the quotient by multiplying with the 128-bit reciprocal M_HIGH and
 * shifting the product's high half by 51.
 */
static StdU128
std_fmt_div_rem_1e16(StdU128 n, uint64 *result_1)
{
	StdU128		d = {0, UINT64CONST(10000000000000000)};
	StdU128		m_high = {UINT64CONST(0x39a5652fb1137856), UINT64CONST(0xd30baf9a1e626a6d)};
	StdU128		zero = {0, 0};
	StdU128		high;
	StdU128		quot;
	StdU128		rem;

	if (std_u128_lt(n, d))
	{
		*result_1 = n.lo;
		return zero;
	}
	(void) std_carrying_mul_u128(n, m_high, zero, &high);
	quot = std_shr_folded_u128(high, 51);
	rem = std_wrapping_sub_u128(n, std_wrapping_mul_u128(quot, d));
	*result_1 = rem.lo;
	return quot;
}

/*
 * std_fmt_u128_fmt_inner is u128::_fmt_inner: its decimal digits written to
 * the end of a buffer of 39, the low sixteen and the next sixteen by
 * enc_16lsd after divisions by 10^16, then four at a time, a pair, and one,
 * returning where they start.
 */
static size_t
std_fmt_u128_fmt_inner(StdU128 self, char *buf, size_t len)
{
	uint64		mod_1e16;
	StdU128		quot_1e16;
	uint64		remain;
	size_t		offset;

	if (self.hi == 0 && self.lo == 0)
	{
		offset = std_wrapping_sub_usize(len, 1);
		std_index_check(offset, len, STD_FMT_NUM_FILE, 660, 13);
		buf[offset] = '0';
		return offset;
	}
	quot_1e16 = std_fmt_div_rem_1e16(self, &mod_1e16);
	if (quot_1e16.hi == 0 && quot_1e16.lo == 0)
	{
		remain = mod_1e16;
		offset = STD_FMT_U128_MAX_DEC_N;
	}
	else
	{
		uint64		mod2;
		StdU128		quot2;

		std_fmt_enc_16lsd(buf, STD_FMT_U128_MAX_DEC_N - 16, mod_1e16);
		quot2 = std_fmt_div_rem_1e16(quot_1e16, &mod2);
		if (quot2.hi == 0 && quot2.lo == 0)
		{
			remain = mod2;
			offset = STD_FMT_U128_MAX_DEC_N - 16;
		}
		else
		{
			std_fmt_enc_16lsd(buf, STD_FMT_U128_MAX_DEC_N - 32, mod2);
			remain = quot2.lo;
			offset = STD_FMT_U128_MAX_DEC_N - 32;
		}
	}
	while (remain > 999)
	{
		uint64		quad;

		offset = std_wrapping_sub_usize(offset, 4);
		quad = STD_REM_FOLDED(u64, remain, 10000);
		remain = STD_DIV_FOLDED(u64, remain, 10000);
		std_fmt_write_quad(buf + offset, 4, quad);
	}
	if (remain > 9)
	{
		size_t		pair_at;

		offset = std_wrapping_sub_usize(offset, 2);
		pair_at = std_wrapping_mul_usize((size_t) STD_REM_FOLDED(u64, remain, 100), 2);
		remain = STD_DIV_FOLDED(u64, remain, 100);
		std_index_check(offset, len, STD_FMT_NUM_FILE, 720, 13);
		std_index_check(pair_at, 200, STD_FMT_NUM_FILE, 720, 35);
		buf[offset] = STD_FMT_DECIMAL_PAIRS[pair_at];
		std_index_check(std_wrapping_add_usize(offset, 1), len, STD_FMT_NUM_FILE, 721, 13);
		std_index_check(std_wrapping_add_usize(pair_at, 1), 200, STD_FMT_NUM_FILE, 721, 35);
		buf[std_wrapping_add_usize(offset, 1)] = STD_FMT_DECIMAL_PAIRS[std_wrapping_add_usize(pair_at, 1)];
	}
	if (remain != 0)
	{
		size_t		last_at;

		offset = std_wrapping_sub_usize(offset, 1);
		last_at = std_wrapping_add_usize(std_wrapping_mul_usize((size_t) (remain & 15), 2), 1);
		std_index_check(offset, len, STD_FMT_NUM_FILE, 737, 13);
		std_index_check(last_at, 200, STD_FMT_NUM_FILE, 737, 31);
		buf[offset] = STD_FMT_DECIMAL_PAIRS[last_at];
	}
	return offset;
}

/* std_u128_Display_fmt is u128's Display: its digits, nonnegative, through pad_integral. */
bool
std_u128_Display_fmt(const void *value, StdFormatter *f)
{
	char		buf[STD_FMT_U128_MAX_DEC_N];
	size_t		offset = std_fmt_u128_fmt_inner(*(const StdU128 *) value, buf, sizeof(buf));
	size_t		digits_len;
	const char *digits = std_fmt_slice_buffer_to_str(buf, sizeof(buf), offset, &digits_len);

	return std_fmt_pad_integral(f, true, "", digits, digits_len);
}

/* std_u128_Debug_fmt is u128's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_u128_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_u128_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_u128_UpperHex_fmt(value, f);
	else
		return std_u128_Display_fmt(value, f);
}

/* std_CellI32_Debug_fmt is Cell<i32>'s Debug: a struct Cell with its value, get's copy. */
bool
std_CellI32_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct b = std_fmt_debug_struct(f, "Cell", 4);
	int32		got = *(const int32 *) value;

	std_DebugStruct_field(&b, "value", 5, &got, std_i32_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* std_u64_Debug_fmt is u64's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_u64_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_u64_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_u64_UpperHex_fmt(value, f);
	else
		return std_u64_Display_fmt(value, f);
}

/* std_NonZeroU64_Debug_fmt is NonZero<u64>'s Debug, impl_nonzero_fmt!'s: its value's Debug. */
bool
std_NonZeroU64_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_u64_Debug_fmt(value, f);
}

/* std_usize_Debug_fmt is usize's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_usize_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_usize_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_usize_UpperHex_fmt(value, f);
	else
		return std_usize_Display_fmt(value, f);
}

/* std_i16_Debug_fmt is i16's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_i16_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_i16_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_i16_UpperHex_fmt(value, f);
	else
		return std_i16_Display_fmt(value, f);
}

/* std_i32_Debug_fmt is i32's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_i32_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_i32_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_i32_UpperHex_fmt(value, f);
	else
		return std_i32_Display_fmt(value, f);
}

/* std_i64_Debug_fmt is i64's Debug, impl_Debug!'s: its LowerHex under {:x?}, its UpperHex under {:X?}, and its Display otherwise. */
bool
std_i64_Debug_fmt(const void *value, StdFormatter *f)
{
	if (f->options.debug_lower_hex)
		return std_i64_LowerHex_fmt(value, f);
	else if (f->options.debug_upper_hex)
		return std_i64_UpperHex_fmt(value, f);
	else
		return std_i64_Display_fmt(value, f);
}

/* std_bool_Display_fmt is bool's Display, which pads "true" or "false". */
bool
std_bool_Display_fmt(const void *value, StdFormatter *f)
{
	return *(const bool *) value ? std_fmt_pad(f, "true", 4) : std_fmt_pad(f, "false", 5);
}

/* std_bool_Debug_fmt is bool's Debug, its Display. */
bool
std_bool_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_bool_Display_fmt(value, f);
}

/* std_str_Display_fmt is str's Display, a pad of a StdStr. */
bool
std_str_Display_fmt(const void *value, StdFormatter *f)
{
	const StdStr *s = (const StdStr *) value;

	return std_fmt_pad(f, s->ptr, s->len);
}

/* std_String_Display_fmt is String's Display, its str's. */
bool
std_String_Display_fmt(const void *value, StdFormatter *f)
{
	const StdString *s = (const StdString *) value;

	return std_fmt_pad(f, (const char *) s->ptr, s->len);
}

/*
 * StdPadAdapter is builders.rs's PadAdapter: the formatter's buffer, which
 * it indents, and its state's on_newline.
 */
typedef struct StdPadAdapter
{
	const StdFmtWriteOps *ops;
	void	   *buf;
	bool	   *on_newline;
} StdPadAdapter;

/*
 * std_PadAdapter_write_str is PadAdapter's write_str: each line of the text,
 * split_inclusive at '\n', after four spaces when a line just started.
 */
static bool
std_PadAdapter_write_str(void *self_, const char *s, size_t len)
{
	StdPadAdapter *self = self_;
	StdSplitInclusiveChar lines = std_str_split_inclusive_char_Unchecked(s, len, '\n');
	const char *line;
	size_t		line_len;

	while (std_str_SplitInclusive_next_Unchecked(&lines, &line, &line_len))
	{
		if (*self->on_newline && !self->ops->write_str(self->buf, "    ", 4))
			return false;
		*self->on_newline = std_str_ends_with_char_Unchecked(line, line_len, '\n');
		if (!self->ops->write_str(self->buf, line, line_len))
			return false;
	}
	return true;
}

/* std_PadAdapter_write_char is PadAdapter's write_char: the char after four spaces when a line just started. */
static bool
std_PadAdapter_write_char(void *self_, uint32 c)
{
	StdPadAdapter *self = self_;

	if (*self->on_newline && !self->ops->write_str(self->buf, "    ", 4))
		return false;
	*self->on_newline = c == '\n';
	return self->ops->write_char(self->buf, c);
}

/* STD_PAD_ADAPTER_OPS is PadAdapter's fmt::Write table. */
static const StdFmtWriteOps STD_PAD_ADAPTER_OPS = {std_PadAdapter_write_str, std_PadAdapter_write_char};

/*
 * std_PadAdapter_wrap is PadAdapter::wrap, Formatter::wrap_buf: a formatter
 * of the same options writing through a PadAdapter over the formatter's
 * buffer, kept in the slot.
 */
static StdFormatter
std_PadAdapter_wrap(StdFormatter *fmt, StdPadAdapter *slot, bool *state)
{
	slot->ops = fmt->ops;
	slot->buf = fmt->buf;
	slot->on_newline = state;
	return (StdFormatter)
	{
		&STD_PAD_ADAPTER_OPS, slot, fmt->options
	};
}

/* std_fmt_debug_struct is Formatter::debug_struct, which writes the name. */
StdDebugStruct
std_fmt_debug_struct(StdFormatter *f, const char *name, size_t len)
{
	return (StdDebugStruct)
	{
		f, std_fmt_write_str(f, name, len), false
	};
}

/*
 * std_DebugStruct_field is DebugStruct::field: " { " or ", ", the name, ": ",
 * and the value, or when pretty " {\n" first, then the name, ": ", the
 * value, and ",\n" through a PadAdapter.
 */
void
std_DebugStruct_field(StdDebugStruct *self, const char *name, size_t len,
					  const void *value, StdFmtFn fmt)
{
	if (self->result)
	{
		if (self->fmt->options.alternate)
		{
			StdPadAdapter slot;
			bool		state = true;
			StdFormatter writer;

			if (!self->has_fields && !std_fmt_write_str(self->fmt, " {\n", 3))
				self->result = false;
			else
			{
				writer = std_PadAdapter_wrap(self->fmt, &slot, &state);
				self->result = std_fmt_write_str(&writer, name, len) &&
					std_fmt_write_str(&writer, ": ", 2) &&
					fmt(value, &writer) &&
					std_fmt_write_str(&writer, ",\n", 2);
			}
		}
		else
		{
			const char *prefix = self->has_fields ? ", " : " { ";

			self->result = std_fmt_write_str(self->fmt, prefix, strlen(prefix)) &&
				std_fmt_write_str(self->fmt, name, len) &&
				std_fmt_write_str(self->fmt, ": ", 2) &&
				fmt(value, self->fmt);
		}
	}
	self->has_fields = true;
}

/* std_DebugStruct_finish is DebugStruct::finish, which closes a struct that had fields, pretty with "}". */
bool
std_DebugStruct_finish(StdDebugStruct *self)
{
	if (self->has_fields && self->result)
		self->result = self->fmt->options.alternate ? std_fmt_write_str(self->fmt, "}", 1) :
			std_fmt_write_str(self->fmt, " }", 2);
	return self->result;
}

/* std_DebugStruct_finish_non_exhaustive is DebugStruct::finish_non_exhaustive, which closes a struct with "..". */
bool
std_DebugStruct_finish_non_exhaustive(StdDebugStruct *self)
{
	if (self->result)
	{
		if (self->has_fields)
		{
			if (self->fmt->options.alternate)
			{
				StdPadAdapter slot;
				bool		state = true;
				StdFormatter writer = std_PadAdapter_wrap(self->fmt, &slot, &state);

				self->result = std_fmt_write_str(&writer, "..\n", 3) && std_fmt_write_str(self->fmt, "}", 1);
			}
			else
				self->result = std_fmt_write_str(self->fmt, ", .. }", 6);
		}
		else
			self->result = std_fmt_write_str(self->fmt, " { .. }", 7);
	}
	return self->result;
}

/* std_unit_Debug_fmt is ()'s Debug, padded. */
bool
std_unit_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_pad(f, "()", 2);
}

/* std_TryFromSliceError_Debug_fmt is core::array::TryFromSliceError's derived Debug: the tuple struct of its (). */
bool
std_TryFromSliceError_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple tuple = std_fmt_debug_tuple(f, "TryFromSliceError", sizeof("TryFromSliceError") - 1);

	std_DebugTuple_field(&tuple, value, std_unit_Debug_fmt);
	return std_DebugTuple_finish(&tuple);
}

/* std_IntErrorKind_Debug_fmt is IntErrorKind's derived Debug: its variant's name. */
bool
std_IntErrorKind_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char *const names[] = {"Empty", "InvalidDigit", "PosOverflow", "NegOverflow", "Zero"};
	const char *name = names[*(const StdIntErrorKind *) value];

	return std_fmt_write_str(f, name, strlen(name));
}

/* std_TryFromIntError_Debug_fmt is TryFromIntError's derived Debug: a tuple of its kind. */
bool
std_TryFromIntError_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdTryFromIntError *self = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "TryFromIntError", 15);

	std_DebugTuple_field(&b, &self->kind, std_IntErrorKind_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* std_ParseIntError_Debug_fmt is ParseIntError's derived Debug: the struct of its kind. */
bool
std_ParseIntError_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdParseIntError *self = value;
	StdDebugStruct b = std_fmt_debug_struct(f, "ParseIntError", 13);

	std_DebugStruct_field(&b, "kind", 4, &self->kind, std_IntErrorKind_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* std_any_Debug_fmt is dyn Any's Debug (core/src/any.rs), a struct named Any whose fields it doesn't show. */
bool
std_any_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugStruct b = std_fmt_debug_struct(f, "Any", 3);

	return std_DebugStruct_finish_non_exhaustive(&b);
}

/* std_fmt_debug_tuple is Formatter::debug_tuple, which writes the name. */
StdDebugTuple
std_fmt_debug_tuple(StdFormatter *f, const char *name, size_t len)
{
	return (StdDebugTuple)
	{
		f, std_fmt_write_str(f, name, len), 0, len == 0
	};
}

/*
 * std_DebugTuple_field is DebugTuple::field: "(" or ", ", and the value, or
 * when pretty "(\n" first, then the value and ",\n" through a PadAdapter.
 */
void
std_DebugTuple_field(StdDebugTuple *self, const void *value, StdFmtFn fmt)
{
	if (self->result)
	{
		if (self->fmt->options.alternate)
		{
			StdPadAdapter slot;
			bool		state = true;
			StdFormatter writer;

			if (self->fields == 0 && !std_fmt_write_str(self->fmt, "(\n", 2))
				self->result = false;
			else
			{
				writer = std_PadAdapter_wrap(self->fmt, &slot, &state);
				self->result = fmt(value, &writer) && std_fmt_write_str(&writer, ",\n", 2);
			}
		}
		else
		{
			const char *prefix = self->fields == 0 ? "(" : ", ";

			self->result = std_fmt_write_str(self->fmt, prefix, strlen(prefix)) &&
				fmt(value, self->fmt);
		}
	}
	self->fields++;
}

/*
 * std_DebugTuple_finish is DebugTuple::finish: a tuple's comma after a lone
 * field of a nameless tuple, and its close.
 */
bool
std_DebugTuple_finish(StdDebugTuple *self)
{
	if (self->result && self->fields > 0)
	{
		if (self->fields == 1 && self->empty_name && !self->fmt->options.alternate)
			self->result = std_fmt_write_str(self->fmt, ",", 1);
		if (self->result)
			self->result = std_fmt_write_str(self->fmt, ")", 1);
	}
	return self->result;
}

/* std_fmt_debug_list is Formatter::debug_list, which writes "[". */
StdDebugList
std_fmt_debug_list(StdFormatter *f)
{
	return (StdDebugList)
	{
		f, std_fmt_write_str(f, "[", 1), false
	};
}

/*
 * std_DebugList_entry is DebugList::entry, DebugInner's: ", " after the
 * first, and the value, or when pretty "\n" first, then the value and ",\n"
 * through a PadAdapter.
 */
void
std_DebugList_entry(StdDebugList *self, const void *value, StdFmtFn fmt)
{
	if (self->result)
	{
		if (self->fmt->options.alternate)
		{
			StdPadAdapter slot;
			bool		state = true;
			StdFormatter writer;

			if (!self->has_fields && !std_fmt_write_str(self->fmt, "\n", 1))
				self->result = false;
			else
			{
				writer = std_PadAdapter_wrap(self->fmt, &slot, &state);
				self->result = fmt(value, &writer) && std_fmt_write_str(&writer, ",\n", 2);
			}
		}
		else
		{
			if (self->has_fields)
				self->result = std_fmt_write_str(self->fmt, ", ", 2);
			if (self->result)
				self->result = fmt(value, self->fmt);
		}
	}
	self->has_fields = true;
}

/* std_DebugList_finish is DebugList::finish, which writes "]". */
bool
std_DebugList_finish(StdDebugList *self)
{
	if (self->result)
		self->result = std_fmt_write_str(self->fmt, "]", 1);
	return self->result;
}

/* std_fmt_debug_set is Formatter::debug_set, which writes "{". */
StdDebugList
std_fmt_debug_set(StdFormatter *f)
{
	StdDebugList set = {f, std_fmt_write_str(f, "{", 1), false};

	return set;
}

/* std_DebugSet_entry is DebugSet::entry, its DebugInner's. */
void
std_DebugSet_entry(StdDebugList *self, const void *value, StdFmtFn fmt)
{
	std_DebugList_entry(self, value, fmt);
}

/* std_DebugSet_finish is DebugSet::finish, which writes "}". */
bool
std_DebugSet_finish(StdDebugList *self)
{
	if (self->result)
		self->result = std_fmt_write_str(self->fmt, "}", 1);
	return self->result;
}

/* STD_FMT_BUILDERS_FILE is core's fmt/builders.rs, where DebugMap's asserts are. */
#define STD_FMT_BUILDERS_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "fmt" STD_SEP "builders.rs"

/* std_fmt_debug_map is Formatter::debug_map, which writes "{". */
StdDebugMap
std_fmt_debug_map(StdFormatter *f)
{
	StdDebugMap map = {f, std_fmt_write_str(f, "{", 1), false, false, true};

	return map;
}

/*
 * std_DebugMap_key is DebugMap::key: ", " after the first entry, the key, and
 * ": ", or when pretty "\n" first, then the key and ": " through a fresh
 * PadAdapter.
 */
static void
std_DebugMap_key(StdDebugMap *self, const void *key, StdFmtFn fmt)
{
	static const char text[] = "attempted to begin a new map entry without completing the previous one";

	if (!self->result)
		return;
	if (self->has_key)
		std_panic(STD_FMT_BUILDERS_FILE, 1001, 13, text, sizeof(text) - 1);
	if (self->fmt->options.alternate)
	{
		StdPadAdapter slot;
		StdFormatter writer;

		if (!self->has_fields && !std_fmt_write_str(self->fmt, "\n", 1))
		{
			self->result = false;
			return;
		}
		self->on_newline = true;
		writer = std_PadAdapter_wrap(self->fmt, &slot, &self->on_newline);
		if (!fmt(key, &writer) || !std_fmt_write_str(&writer, ": ", 2))
		{
			self->result = false;
			return;
		}
	}
	else
	{
		if ((self->has_fields && !std_fmt_write_str(self->fmt, ", ", 2)) || !fmt(key, self->fmt) ||
			!std_fmt_write_str(self->fmt, ": ", 2))
		{
			self->result = false;
			return;
		}
	}
	self->has_key = true;
}

/*
 * std_DebugMap_value is DebugMap::value: the value, or when pretty the value
 * and ",\n" through a PadAdapter of the key's state.
 */
static void
std_DebugMap_value(StdDebugMap *self, const void *value, StdFmtFn fmt)
{
	static const char text[] = "attempted to format a map value before its key";

	if (self->result)
	{
		if (!self->has_key)
			std_panic(STD_FMT_BUILDERS_FILE, 1077, 13, text, sizeof(text) - 1);
		if (self->fmt->options.alternate)
		{
			StdPadAdapter slot;
			StdFormatter writer = std_PadAdapter_wrap(self->fmt, &slot, &self->on_newline);

			self->result = fmt(value, &writer) && std_fmt_write_str(&writer, ",\n", 2);
		}
		else
			self->result = fmt(value, self->fmt);
		if (self->result)
			self->has_key = false;
	}
	self->has_fields = true;
}

/* std_DebugMap_entry is DebugMap::entry: the key, then the value. */
void
std_DebugMap_entry(StdDebugMap *self, const void *key, StdFmtFn key_fmt, const void *value, StdFmtFn value_fmt)
{
	std_DebugMap_key(self, key, key_fmt);
	std_DebugMap_value(self, value, value_fmt);
}

/* std_DebugMap_finish is DebugMap::finish, which writes "}". */
bool
std_DebugMap_finish(StdDebugMap *self)
{
	static const char text[] = "attempted to finish a map with a partial entry";

	if (self->result)
	{
		if (self->has_key)
			std_panic(STD_FMT_BUILDERS_FILE, 1229, 13, text, sizeof(text) - 1);
		self->result = std_fmt_write_str(self->fmt, "}", 1);
	}
	return self->result;
}

/* std_fmt_needs_escape is str Debug's test of a byte that its fast path can't pass. */
static bool
std_fmt_needs_escape(uint8 b)
{
	return b > 0x7E || b < 0x20 || b == '\\' || b == '"';
}

/*
 * std_fmt_str_debug is str's Debug: the text in double quotes, runs of printable
 * ASCII written as they are and each char that needs one by its escape.
 */
static bool
std_fmt_str_debug(const char *self, size_t len, StdFormatter *f)
{
	size_t		start = 0;
	size_t		end = 0;
	size_t		rest = 0;

	if (!std_fmt_write_char(f, '"'))
		return false;
	while (rest < len)
	{
		size_t		nps = 0;
		bool		found = false;
		StdChars	chars;
		uint32		c;
		char		buf[16];
		size_t		buf_len;

		for (size_t i = rest; i < len; i++)
		{
			if (std_fmt_needs_escape((uint8) self[i]))
			{
				nps = i - rest;
				found = true;
				break;
			}
		}
		if (!found)
		{
			end += len - rest;
			break;
		}
		end += nps;
		rest += nps;
		chars = std_str_chars(self + rest, len - rest);
		if (std_str_Chars_next_Unchecked(&chars, &c))
		{
			if (std_char_escape_debug_ext(c, true, false, true, buf, &buf_len) != 1)
			{
				std_str_index_range(self, len, start, end, STD_FMT_MOD_FILE, 2949, 38);
				if (!std_fmt_write_str(f, self + start, end - start))
					return false;
				if (!std_fmt_write_str(f, buf, buf_len))
					return false;
				start = end + std_char_len_utf8(c);
			}
			end += std_char_len_utf8(c);
		}
		rest = (size_t) ((const char *) chars.iter.ptr - self);
	}
	std_str_index_range(self, len, start, end, STD_FMT_MOD_FILE, 2958, 26);
	if (!std_fmt_write_str(f, self + start, end - start))
		return false;
	return std_fmt_write_char(f, '"');
}

/* std_str_Debug_fmt is str's Debug, of a StdStr. */
bool
std_str_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdStr *s = (const StdStr *) value;

	return std_fmt_str_debug(s->ptr, s->len, f);
}

/* std_String_Debug_fmt is String's Debug, its str's. */
bool
std_String_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdString *s = (const StdString *) value;

	return std_fmt_str_debug((const char *) s->ptr, s->len, f);
}

/* std_bytes_Debug_fmt is a &[u8]'s Debug, of a StdBytes: its bytes as a list. */
bool
std_bytes_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdBytes *bytes = (const StdBytes *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < bytes->len; i++)
		std_DebugList_entry(&b, &bytes->ptr[i], std_u8_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* std_tuple_Debug_fmt is a tuple's Debug: its items as an unnamed debug_tuple, "(a, b)". */
bool
std_tuple_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdTupleRef *self = (const StdTupleRef *) value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	for (size_t i = 0; i < self->len; i++)
		std_DebugTuple_field(&b, self->items[i].value, self->items[i].fmt);
	return std_DebugTuple_finish(&b);
}

/* std_option_Debug_fmt is Option's derived Debug: None, or Some with its value. */
bool
std_option_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdOptionRef *self = (const StdOptionRef *) value;
	StdDebugTuple b;

	if (!self->has)
		return std_fmt_write_str(f, "None", 4);
	b = std_fmt_debug_tuple(f, "Some", 4);
	std_DebugTuple_field(&b, self->some.value, self->some.fmt);
	return std_DebugTuple_finish(&b);
}

/* std_fmt_Arguments_Display_fmt is fmt::Arguments' Display: the arguments written to the formatter's output. */
static bool
std_fmt_Arguments_Display_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_write(f->ops, f->buf, value);
}

/*
 * std_assert_failed is core::panicking::assert_failed with its message, NULL for none: the
 * panic of assert_eq! or assert_ne! at its caller, whose payload is formatted
 * into a String as a formatted panic's is.
 */
void
std_assert_failed(const char *file, uint32 line, uint32 col, const char *op, const void *left,
				  StdFmtFn left_fmt, const void *right, StdFmtFn right_fmt, const StdFmtArguments *args)
{
	StdStr		op_str = {op, strlen(op)};

	if (args != NULL)
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("assertion `left "), STD_FMT_ARG(0), STD_FMT_LIT(" right` failed: "), STD_FMT_ARG(1),
			STD_FMT_LIT("\n  left: "), STD_FMT_ARG(2), STD_FMT_LIT("\n right: "), STD_FMT_ARG(3),
		};
		StdFmtArg	fmt_args[] = {{&op_str, std_str_Display_fmt}, {args, std_fmt_Arguments_Display_fmt},
		{left, left_fmt}, {right, right_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), fmt_args};

		std_panic_fmt(file, line, col, &arguments);
	}
	else
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("assertion `left "), STD_FMT_ARG(0), STD_FMT_LIT(" right` failed\n  left: "),
			STD_FMT_ARG(1), STD_FMT_LIT("\n right: "), STD_FMT_ARG(2),
		};
		StdFmtArg	fmt_args[] = {{&op_str, std_str_Display_fmt}, {left, left_fmt}, {right, right_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), fmt_args};

		std_panic_fmt(file, line, col, &arguments);
	}
}
