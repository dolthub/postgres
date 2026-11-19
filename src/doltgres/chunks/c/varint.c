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
 * Variable-length unsigned integers: seven bits per byte, low bits first,
 * with the high bit set on every byte but the last. Verus proves the Rust's
 * put_varint, varint_len, and decode against their specification; the C's
 * guarantee is tested equivalence.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/varint.h"
#include "doltgres/std/num.h"
#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "varint.rs"

/* chunks_varint_put_varint appends a varint. */
void
chunks_varint_put_varint(StdVec *out, uint64 value)
{
	while (value >= 0x80)
	{
		std_vec_u8_push_Checked(out, (uint8) ((uint8) value | 0x80));
		value = STD_SHR_FOLDED(u64, value, 7);
	}
	std_vec_u8_push_Checked(out, (uint8) value);
}

/* chunks_varint_varint_len returns the number of bytes the value takes as a varint. */
size_t
chunks_varint_varint_len(uint64 value)
{
	size_t		len = 1;
	uint64		rest = value;

	while (rest >= 0x80)
	{
		rest = STD_SHR_FOLDED(u64, rest, 7);
		len = STD_ADD(usize, len, 1, 132, 9);
	}
	return len;
}

/*
 * chunks_varint_decode_long is decode's loop for a varint that takes more
 * than one byte or runs past the data.
 */
static bool
chunks_varint_decode_long(const uint8 *bytes, size_t bytes_len, size_t *at, uint64 *result,
						  ChunksVarintMalformed *error)
{
	uint64		value = 0;
	uint32		shift = 0;
	size_t		i = 0;

	while (i < 10)
	{
		uint8		byte;
		uint64		low;

		if (*at >= bytes_len)
		{
			*error = CHUNKS_VARINT_MALFORMED_PAST_END;
			return false;
		}
		std_index_check(*at, bytes_len, RUST_FILE, 416, 20);
		byte = bytes[*at];
		*at = STD_ADD(usize, *at, 1, 420, 9);
		if (shift == 63 && byte > 1)
		{
			*error = CHUNKS_VARINT_MALFORMED_TOO_LARGE;
			return false;
		}
		low = (uint64) (uint8) (byte & 0x7f);
		value |= STD_SHL(u64, low, shift, 433, 18);
		if ((uint8) (byte & 0x80) == 0)
		{
			if (byte == 0 && shift > 0)
			{
				*error = CHUNKS_VARINT_MALFORMED_NOT_SHORTEST;
				return false;
			}
			*result = value;
			return true;
		}
		shift = STD_ADD(u32, shift, 7, 459, 9);
		i = STD_ADD(usize, i, 1, 460, 9);
	}
	*error = CHUNKS_VARINT_MALFORMED_TOO_LARGE;
	return false;
}

/*
 * chunks_varint_decode reads a varint at a position and moves the position
 * past it, or says why the bytes hold none.
 */
bool
chunks_varint_decode(const uint8 *bytes, size_t bytes_len, size_t *at,
					 uint64 *result, ChunksVarintMalformed *error)
{
	bool		short_ = *at < bytes_len;

	if (short_)
	{
		std_index_check(*at, bytes_len, RUST_FILE, 343, 29);
		short_ = bytes[*at] < 0x80;
	}
	if (short_)
	{
		uint8		byte;

		std_index_check(*at, bytes_len, RUST_FILE, 344, 20);
		byte = bytes[*at];
		*at = STD_ADD(usize, *at, 1, 352, 9);
		*result = (uint64) byte;
		return true;
	}
	return chunks_varint_decode_long(bytes, bytes_len, at, result, error);
}

/*
 * chunks_varint_read_varint reads a varint at a position and moves the
 * position past it. A varint must be written in its shortest form, so that
 * every value has exactly one encoding.
 */
bool
chunks_varint_read_varint(const uint8 *bytes, size_t bytes_len, size_t *at,
						  uint64 *result, ChunksErrorError *error)
{
	ChunksVarintMalformed malformed;

	if (chunks_varint_decode(bytes, bytes_len, at, result, &malformed))
		return true;
	*error = chunks_varint_Malformed_error(malformed);
	return false;
}

/* chunks_varint_Malformed_error returns the error of bytes malformed so. */
ChunksErrorError
chunks_varint_Malformed_error(ChunksVarintMalformed self)
{
	StdString	message;
	StdStr text = {0};

	switch (self)
	{
		case CHUNKS_VARINT_MALFORMED_PAST_END:
			text = STD_STR("varint runs past the end of its data");

			break;
		case CHUNKS_VARINT_MALFORMED_TOO_LARGE:
			text = STD_STR("varint is too large");

			break;
		case CHUNKS_VARINT_MALFORMED_NOT_SHORTEST:
			text = STD_STR("varint is not in its shortest form");

			break;
	}
	std_string_from_Checked(&message, text.ptr, text.len);
	return chunks_error_Error_corrupt(&message);
}

#ifdef DOLTGRES_TEST_CHUNKS

/*
 * test_chunks_varint_tests_round_trips writes each of the values a varint's
 * forms meet at their edges and reads it back: its length is varint_len's,
 * and the read ends where the write did.
 */
void
test_chunks_varint_tests_round_trips(void)
{
	static const uint64 values[] = {0, 1, 127, 128, 255, 16383, 16384, PG_UINT32_MAX, PG_UINT64_MAX - 1, PG_UINT64_MAX};

	for (size_t i = 0; i < lengthof(values); i++)
	{
		uint64		value = values[i];
		StdVec		out = STD_VEC_NEW;
		size_t		len;
		size_t		at = 0;
		uint64		read;
		ChunksErrorError error;

		chunks_varint_put_varint(&out, value);
		len = chunks_varint_varint_len(value);
		STD_ASSERT_EQ(565, 13, out.len == len, &out.len, std_usize_Debug_fmt, &len, std_usize_Debug_fmt);
		if (!chunks_varint_read_varint(out.ptr, out.len, &at, &read, &error))
			STD_RESULT_EXPECT_FAILED(567, 48, "called `Result::unwrap()` on an `Err` value", &error,
									 chunks_error_Error_Debug_fmt);
		STD_ASSERT_EQ(567, 13, read == value, &read, std_u64_Debug_fmt, &value, std_u64_Debug_fmt);
		STD_ASSERT_EQ(568, 13, at == out.len, &at, std_usize_Debug_fmt, &out.len, std_usize_Debug_fmt);
		std_vec_free_Checked(&out, 1, 1);
	}
}

/* chunks_varint_tests_refused is whether a read of bytes fails, its error dropped. */
static bool
chunks_varint_tests_refused(const uint8 *bytes, size_t len)
{
	size_t		at = 0;
	uint64		read;
	ChunksErrorError error;

	if (chunks_varint_read_varint(bytes, len, &at, &read, &error))
		return false;
	chunks_error_Error_drop(&error);
	return true;
}

/*
 * test_chunks_varint_tests_rejects_long_forms refuses a varint's longer
 * form of a value, one cut short, and one past ten bytes.
 */
void
test_chunks_varint_tests_rejects_long_forms(void)
{
	static const uint8 long_zero[] = {0x80, 0x00};
	static const uint8 cut[] = {0x80};
	static const uint8 past[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	STD_ASSERT(574, 9, chunks_varint_tests_refused(long_zero, sizeof(long_zero)),
			   "read_varint(&[0x80, 0x00], &mut 0).is_err()");
	STD_ASSERT(575, 9, chunks_varint_tests_refused(cut, sizeof(cut)), "read_varint(&[0x80], &mut 0).is_err()");
	STD_ASSERT(576, 9, chunks_varint_tests_refused(past, sizeof(past)), "read_varint(&[0xff; 10], &mut 0).is_err()");
}

#endif							/* DOLTGRES_TEST_CHUNKS */
