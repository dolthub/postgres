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
 * CString (the port text's section 9), alloc's ffi/c_str.rs: new from a
 * String, Default, as_ptr, and the drops of a CString and a NulError.
 * CString::new is generic, so the crate calling it codegens its checks, while
 * _from_vec_unchecked and Default are alloc's own code, which runs none.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/c_str.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/str.h"

/* C_STR_FILE is alloc's ffi/c_str.rs, where CString's checks report. */
#define C_STR_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "ffi" STD_SEP "c_str.rs"

/*
 * std_CString_from_vec_unchecked is CString::_from_vec_unchecked: room for
 * exactly one more byte, its NUL pushed, and the buffer shrunk into the box.
 */
static StdCString
std_CString_from_vec_unchecked(StdVec *v)
{
	StdCString	c;

	std_vec_reserve_exact_Unchecked(v, 1, 1, 1);
	std_vec_u8_push_Unchecked(v, 0);
	c.ptr = std_vec_into_boxed_slice_Unchecked(v, 1, 1, &c.len);
	return c;
}

/*
 * std_CString_new_String is CString::new of a String, SpecNewImpl's default:
 * the String's bytes taken as a Vec<u8>, NulError of the first NUL's position
 * and the bytes, else _from_vec_unchecked's CString.
 */
bool
std_CString_new_String(StdString *t, StdCString *result, StdNulError *error)
{
	StdVec		bytes = *t;
	size_t		i;

	if (std_str_memchr_Checked(0, (const uint8 *) bytes.ptr, bytes.len, &i))
	{
		error->pos = i;
		error->bytes = bytes;
		return false;
	}
	*result = std_CString_from_vec_unchecked(&bytes);
	return true;
}

/*
 * std_CString_new_bytes is CString::new of a &[u8], spec_new_impl_bytes: a
 * buffer of exactly one more byte than the bytes, the bytes copied into it,
 * then NulError of the first NUL's position and the buffer, else
 * _from_vec_unchecked's CString.
 */
bool
std_CString_new_bytes(const uint8 *bytes, size_t len, StdCString *result, StdNulError *error)
{
	size_t		capacity;
	StdVec		buffer;
	size_t		i;

	if (!std_checked_add_usize(len, 1, &capacity))
		std_option_unwrap_failed(C_STR_FILE, 279, 55);
	std_vec_with_capacity_Checked(&buffer, capacity, 1, 1);
	std_vec_extend_from_slice_Checked(&buffer, bytes, len, 1, 1);
	if (std_str_memchr_Checked(0, bytes, len, &i))
	{
		error->pos = i;
		error->bytes = buffer;
		return false;
	}
	*result = std_CString_from_vec_unchecked(&buffer);
	return true;
}

/*
 * std_CString_default is CString's Default: the empty CStr's to_owned, a box
 * of its one NUL, which Box::clone_from_ref allocates for its layout, an
 * allocation that fails being handle_alloc_error's.
 */
void
std_CString_default(StdCString *result)
{
	uint8	   *ptr = std_box_new_uninit_layout_Unchecked(1, 1);

	ptr[0] = 0;
	result->ptr = ptr;
	result->len = 1;
}

/* std_CString_as_ptr is CString's as_ptr: its bytes, NUL-terminated. */
const char *
std_CString_as_ptr(const StdCString *self)
{
	return (const char *) self->ptr;
}

/*
 * std_CString_drop is CString's drop, which the crate that drops it codegens:
 * its first byte zeroed through get_unchecked_mut, then its box's free, of
 * Layout::for_value_raw's checked layout.
 */
void
std_CString_drop(StdCString *self)
{
	std_slice_get_unchecked_mut_check(0, self->len, C_STR_FILE, 706, 25);
	self->ptr[0] = 0;
	std_Layout_for_value_raw_check(self->len, 1);
	std_free(self->ptr, self->len, 1);
}

/* std_NulError_drop is NulError's drop: its bytes' Vec<u8>. */
void
std_NulError_drop(StdNulError *self)
{
	std_vec_free_Checked(&self->bytes, 1, 1);
}

/* std_NulError_Debug_fmt is NulError's derived Debug: NulError(position, bytes). */
bool
std_NulError_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdNulError *self = value;
	StdDebugTuple t = std_fmt_debug_tuple(f, "NulError", 8);

	std_DebugTuple_field(&t, &self->pos, std_usize_Debug_fmt);
	std_DebugTuple_field(&t, &self->bytes, std_bytes_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* std_CString_into_bytes_with_nul is CString::into_bytes_with_nul: its Box<[u8]>'s into_vec, its NUL kept. */
void
std_CString_into_bytes_with_nul(StdCString *self, StdVec *result)
{
	std_box_slice_into_vec_Unchecked(result, self->ptr, self->len, 1);
}

/*
 * std_CStr_from_bytes_until_nul is CStr::from_bytes_until_nul: the C string
 * up to and with the first NUL, or FromBytesUntilNulError when the bytes hold
 * none.
 */
bool
std_CStr_from_bytes_until_nul(const uint8 *bytes, size_t len, const char **result,
							  StdFromBytesUntilNulError *error)
{
	size_t		nul_pos;

	if (!std_str_memchr_Unchecked(0, bytes, len, &nul_pos))
		return false;
	*result = (const char *) bytes;
	return true;
}

/* std_FromBytesUntilNulError_Debug_fmt is FromBytesUntilNulError's derived Debug. */
bool
std_FromBytesUntilNulError_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple b = std_fmt_debug_tuple(f, "FromBytesUntilNulError", 22);

	std_DebugTuple_field(&b, value, std_unit_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/*
 * std_CStr_to_owned is CStr's ToOwned: a CString of its bytes and their NUL,
 * their Box<[u8]>'s From<&[u8]>, Box::clone_from_ref, which allocates for
 * their layout and copies them in, an allocation that fails being
 * handle_alloc_error's.
 */
void
std_CStr_to_owned(const char *self, StdCString *result)
{
	size_t		len = std_wrapping_add_usize(strlen(self), 1);
	uint8	   *ptr = std_box_new_uninit_layout_Unchecked(len, 1);

	std_ptr_copy_nonoverlapping_Unchecked(self, ptr, len, 1, 1);
	result->ptr = ptr;
	result->len = len;
}

/*
 * std_CStr_Debug_fmt is CStr's Debug, its value the NUL-ended string: its
 * bytes between double quotes, each as <[u8]>::escape_ascii escapes it, the
 * tab, carriage return, newline, backslash, and both quotes as their
 * backslash escapes, printable ASCII as itself, and any other byte as \x and
 * two lowercase hex digits.
 */
bool
std_CStr_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char hex[] = "0123456789abcdef";

	if (!std_fmt_write_char(f, '"'))
		return false;
	for (const uint8 *b = value; *b != 0; b++)
	{
		char		escaped[4] = {'\\', 0, 0, 0};
		size_t		len = 2;

		switch (*b)
		{
			case '\t':
				escaped[1] = 't';
				break;
			case '\r':
				escaped[1] = 'r';
				break;
			case '\n':
				escaped[1] = 'n';
				break;
			case '\\':
			case '\'':
			case '"':
				escaped[1] = (char) *b;
				break;
			default:
				if (*b >= 0x20 && *b <= 0x7e)
				{
					escaped[0] = (char) *b;
					len = 1;
				}
				else
				{
					escaped[1] = 'x';
					escaped[2] = hex[*b >> 4];
					escaped[3] = hex[*b & 0xf];
					len = 4;
				}
				break;
		}
		if (!std_fmt_write_str(f, escaped, len))
			return false;
	}
	return std_fmt_write_char(f, '"');
}
