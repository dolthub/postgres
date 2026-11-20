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
 * Chunk addresses: the BLAKE3 hash of a chunk's hashed bytes, written as 52
 * base32 characters.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "blake3.h"
#include "doltgres/chunks/address.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "address.rs"

/* CHUNKS_ADDRESS_ALPHABET is the base32 alphabet addresses are written in, Dolt's. */
static const uint8 CHUNKS_ADDRESS_ALPHABET[32] = "0123456789abcdefghijklmnopqrstuv";

/* chunks_address_Address_PartialEq_eq is Address's derived PartialEq, its bytes in order. */
bool
chunks_address_Address_PartialEq_eq(const void *self_, const void *other_)
{
	const ChunksAddressAddress *self = self_;
	const ChunksAddressAddress *other = other_;

	return memcmp(self->f0, other->f0, 32) == 0;
}

/*
 * chunks_address_Address_PartialOrd_partial_cmp is Address's derived
 * PartialOrd, its bytes in order, which orders every pair.
 */
bool
chunks_address_Address_PartialOrd_partial_cmp(const void *self_, const void *other_, StdOrdering *result)
{
	const ChunksAddressAddress *self = self_;
	const ChunksAddressAddress *other = other_;

	*result = std_bytes_Ord_cmp(self->f0, 32, other->f0, 32);
	return true;
}

/* chunks_address_Address_PartialOrd_lt is Address's PartialOrd::lt: whether partial_cmp orders it Less. */
bool
chunks_address_Address_PartialOrd_lt(const void *self, const void *other)
{
	StdOrdering order;

	return chunks_address_Address_PartialOrd_partial_cmp(self, other, &order) && order == STD_ORDERING_LESS;
}

/* chunks_address_Address_PartialOrd_le is Address's PartialOrd::le: whether partial_cmp orders it Less or Equal. */
bool
chunks_address_Address_PartialOrd_le(const void *self, const void *other)
{
	StdOrdering order;

	return chunks_address_Address_PartialOrd_partial_cmp(self, other, &order) && order != STD_ORDERING_GREATER;
}

/* chunks_address_Address_PartialOrd_ge is Address's PartialOrd::ge: whether partial_cmp orders it Greater or Equal. */
bool
chunks_address_Address_PartialOrd_ge(const void *self, const void *other)
{
	StdOrdering order;

	return chunks_address_Address_PartialOrd_partial_cmp(self, other, &order) && order != STD_ORDERING_LESS;
}

/* chunks_address_Address_PartialEq_ne is Address's PartialEq::ne: whether eq finds the addresses differ. */
bool
chunks_address_Address_PartialEq_ne(const void *self, const void *other)
{
	return !chunks_address_Address_PartialEq_eq(self, other);
}

/* chunks_address_Address_Ord_cmp is Address's derived Ord, its bytes in order. */
StdOrdering
chunks_address_Address_Ord_cmp(const void *self_, const void *other_)
{
	const ChunksAddressAddress *self = self_;
	const ChunksAddressAddress *other = other_;

	return std_bytes_Ord_cmp(self->f0, 32, other->f0, 32);
}

/* chunks_address_Address_of returns the address of the hashed bytes. */
ChunksAddressAddress
chunks_address_Address_of(const uint8 *bytes, size_t bytes_len)
{
	blake3_hasher hasher;
	ChunksAddressAddress address;

	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, bytes, bytes_len);
	blake3_hasher_finalize(&hasher, address.f0, 32);
	return address;
}

/* chunks_address_Address_before reports whether the address comes before another in byte order. */
bool
chunks_address_Address_before(const ChunksAddressAddress *self, const ChunksAddressAddress *other)
{
	size_t		i = 0;

	while (i < 32)
	{
		std_index_check(i, 32, RUST_FILE, 139, 16);
		std_index_check(i, 32, RUST_FILE, 139, 29);
		if (self->f0[i] != other->f0[i])
		{
			std_index_check(i, 32, RUST_FILE, 140, 24);
			std_index_check(i, 32, RUST_FILE, 140, 36);
			return self->f0[i] < other->f0[i];
		}
		i = STD_ADD(usize, i, 1, 142, 13);
	}
	return false;
}

/* chunks_address_Address_same reports whether two addresses are the same bytes. */
bool
chunks_address_Address_same(const ChunksAddressAddress *self, const ChunksAddressAddress *other)
{
	size_t		i = 0;

	while (i < 32)
	{
		std_index_check(i, 32, RUST_FILE, 159, 16);
		std_index_check(i, 32, RUST_FILE, 159, 29);
		if (self->f0[i] != other->f0[i])
			return false;
		i = STD_ADD(usize, i, 1, 162, 13);
	}
	return true;
}

/* chunks_address_Address_Hash_hash hashes an address by its first eight bytes. */
void
chunks_address_Address_Hash_hash(const void *self_, StdHasher state)
{
	const ChunksAddressAddress *self = self_;

	std_range_to_check(8, 32, RUST_FILE, 179, 50);
	if (8 != 8)
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt, RUST_FILE, 179, 67);
	state.ops->write_u64(state.self, std_u64_from_le_bytes(self->f0));
}

/*
 * chunks_address_Address_is_empty reports whether the address is all zeros,
 * which no chunk has and which stands for no chunk.
 */
bool
chunks_address_Address_is_empty(const ChunksAddressAddress *self)
{
	static const uint8 zero[32] = {0};

	return memcmp(self->f0, zero, 32) == 0;
}

static bool chunks_address_decode_bits(const char *text, size_t text_len, uint8 *result);

/* chunks_address_Address_parse reads an address written out in full. */
bool
chunks_address_Address_parse(const char *text, size_t text_len, ChunksAddressAddress *result)
{
	uint8		bits[33];

	if (!chunks_address_decode_bits(text, text_len, bits))
		return false;
	if (text_len == CHUNKS_ADDRESS_TEXT_LEN && bits[32] == 0)
	{
		std_range_to_check(32, 33, RUST_FILE, 195, 81);
		if (32 != 32)
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
									 std_TryFromSliceError_Debug_fmt, RUST_FILE, 195, 99);
		memcpy(result->f0, bits, 32);
		return true;
	}
	return false;
}

/*
 * chunks_address_Address_prefix_range returns the lowest and highest
 * addresses whose text starts with the prefix, or None when the prefix isn't
 * base32 or is longer than an address.
 */
bool
chunks_address_Address_prefix_range(const char *prefix, size_t prefix_len,
									chunks_TupleAddressAddress_t *result)
{
	uint8		bits[33];
	uint8		low[32];
	uint8		high[32];
	size_t		from;

	if (prefix_len > CHUNKS_ADDRESS_TEXT_LEN)
		return false;
	if (!chunks_address_decode_bits(prefix, prefix_len, bits))
		return false;
	std_range_to_check(32, 33, RUST_FILE, 204, 49);
	if (32 != 32)
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt, RUST_FILE, 204, 67);
	memcpy(low, bits, 32);
	memcpy(high, low, 32);
	from = std_min_usize(STD_MUL(usize, prefix_len, 5, 206, 21), 256);
	for (size_t bit = from, bit_end = 256; bit < bit_end; bit = std_forward_unchecked_usize_Checked(bit, 1))
	{
		uint8		tmp_1 = STD_SHR(u8, 0x80, (uint32) STD_REM_FOLDED(usize, bit, 8), 207, 30);
		size_t		tmp_2 = STD_DIV_FOLDED(usize, bit, 8);

		std_index_check(tmp_2, 32, RUST_FILE, 207, 13);
		high[tmp_2] |= tmp_1;
	}
	memcpy(result->f0.f0, low, 32);
	memcpy(result->f1.f0, high, 32);
	return true;
}

/* chunks_address_Address_has_prefix reports whether the address written out starts with the text. */
bool
chunks_address_Address_has_prefix(const ChunksAddressAddress *self, const char *prefix, size_t prefix_len)
{
	volatile StdString text;
	volatile bool text_live = false;
	bool		starts = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdString  *text_v = (StdString *) &text;

		std_to_string(self, chunks_address_Address_Display_fmt, text_v);
		text_live = true;
		starts = text_v->len >= prefix_len && memcmp(text_v->ptr, prefix, prefix_len) == 0;
		text_live = false;
		std_vec_free_Checked(text_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (text_live)
			std_vec_free_Checked((StdString *) &text, 1, 1);
		std_unwind_resume(&unwind);
	}
	return starts;
}

/*
 * chunks_address_decode_bits reads base32 text into bytes, five bits per
 * character from the high bit down, with one extra byte for the bits past
 * the 32nd byte.
 */
static bool
chunks_address_decode_bits(const char *text, size_t text_len, uint8 *result)
{
	uint8		out[33] = {0};
	size_t		count = 0;

	for (size_t n = 0, n_len = text_len; n < n_len; n++)
	{
		size_t		i = count;
		uint8		c = (uint8) text[n];
		bool		found = false;
		uint32		value = 0;
		size_t		bit;
		uint32		window;
		size_t		tmp_1;

		count = std_Enumerate_count_step(count);
		for (size_t a = 0, a_len = 32; a < a_len; a++)
		{
			if (CHUNKS_ADDRESS_ALPHABET[a] == c)
			{
				value = (uint32) a;
				found = true;
				break;
			}
		}
		if (!found)
			return false;
		bit = STD_MUL(usize, i, 5, 224, 19);
		if (bit >= 264)
			return false;
		window = STD_SHR(u32, STD_SHL_FOLDED(u32, value, 11), (uint32) STD_REM_FOLDED(usize, bit, 8),
						 228, 22);
		std_index_check(STD_DIV_FOLDED(usize, bit, 8), 33, RUST_FILE, 229, 9);
		out[STD_DIV_FOLDED(usize, bit, 8)] |= (uint8) STD_SHR_FOLDED(u32, window, 8);
		tmp_1 = STD_ADD(usize, STD_DIV_FOLDED(usize, bit, 8), 1, 230, 12);
		if (tmp_1 < 33)
		{
			tmp_1 = STD_ADD(usize, STD_DIV_FOLDED(usize, bit, 8), 1, 231, 17);
			std_index_check(tmp_1, 33, RUST_FILE, 231, 13);
			out[tmp_1] |= (uint8) window;
		}
	}
	memcpy(result, out, 33);
	return true;
}

/* chunks_address_Address_Display_fmt writes the address as its 52 base32 characters. */
bool
chunks_address_Address_Display_fmt(const void *value, StdFormatter *f)
{
	const ChunksAddressAddress *self = (const ChunksAddressAddress *) value;
	uint8 text[ CHUNKS_ADDRESS_TEXT_LEN] = {0};
	size_t		count = 0;
	StdUtf8Error error;

	for (size_t n = 0, n_len = CHUNKS_ADDRESS_TEXT_LEN; n < n_len; n++)
	{
		size_t		i = count;
		size_t		bit;
		uint32		hi;
		uint32		lo;
		uint32		window;
		size_t		at;
		uint32		tmp_1;

		count = std_Enumerate_count_step(count);
		bit = STD_MUL(usize, i, 5, 241, 23);
		std_index_check(STD_DIV_FOLDED(usize, bit, 8), 32, RUST_FILE, 242, 22);
		hi = self->f0[STD_DIV_FOLDED(usize, bit, 8)];
		at = STD_ADD(usize, STD_DIV_FOLDED(usize, bit, 8), 1, 243, 33);
		lo = at < 32 ? self->f0[at] : 0;
		tmp_1 = (uint32) STD_SUB(usize, 11, STD_REM_FOLDED(usize, bit, 8), 244, 45);
		window = STD_SHR(u32, STD_SHL_FOLDED(u32, hi, 8) | lo, tmp_1, 244, 26);
		std_index_check(window & 31, 32, RUST_FILE, 245, 18);
		text[		n] = CHUNKS_ADDRESS_ALPHABET[window & 31];
	}
	if (!std_str_from_utf8(text, CHUNKS_ADDRESS_TEXT_LEN, &error))
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
								 std_Utf8Error_Debug_fmt, RUST_FILE, 247, 48);
	return std_fmt_write_str(f, (const char *) text, CHUNKS_ADDRESS_TEXT_LEN);
}

/* chunks_address_Address_Debug_fmt is Address's Debug, its Display. */
bool
chunks_address_Address_Debug_fmt(const void *value, StdFormatter *f)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{value, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};

	return std_fmt_write_fmt_Checked(f, &arguments);
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_address_tests_text returns an address's text, its Display. */
static StdString
chunks_address_tests_text(const ChunksAddressAddress *address)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{address, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};
	StdString text;

	std_fmt_format_Checked(&text, &arguments);
	return text;
}

/* chunks_address_tests_parsed is Address::parse's Option<Address> of a text, with its Debug's view. */
typedef struct ChunksAddressTestsParsed
{
	bool		has;
	ChunksAddressAddress address;
} ChunksAddressTestsParsed;

/* chunks_address_tests_parse parses a text as Address::parse does. */
static ChunksAddressTestsParsed
chunks_address_tests_parse(const char *text, size_t len)
{
	ChunksAddressTestsParsed parsed = {0};

	parsed.has = chunks_address_Address_parse(text, len, &parsed.address);
	return parsed;
}

/* chunks_address_tests_parsed_eq is Option<Address>'s equality. */
static bool
chunks_address_tests_parsed_eq(const ChunksAddressTestsParsed *a, const ChunksAddressTestsParsed *b)
{
	return a->has == b->has && (!a->has || chunks_address_Address_PartialEq_eq(&a->address, &b->address));
}

/* chunks_address_tests_parsed_Debug_fmt is Option<Address>'s Debug. */
static bool
chunks_address_tests_parsed_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksAddressTestsParsed *parsed = value;
	StdOptionRef option = {parsed->has, {&parsed->address, chunks_address_Address_Debug_fmt}};

	return std_option_Debug_fmt(&option, f);
}

/* chunks_address_tests_string_eq is whether two Strings hold the same text. */
static bool
chunks_address_tests_string_eq(const StdString *a, const StdString *b)
{
	return a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
}

/* chunks_address_tests_vs returns n "v"s and then a last character, as format!("{}g", "v".repeat(51)) makes it. */
static StdString
chunks_address_tests_vs(char last)
{
	StdString text;

	std_vec_with_capacity_Checked(&text, 52, 1, 1);
	memset(text.ptr, 'v', 51);
	((char *) text.ptr)[51] = last;
	text.		len = 52;
	return text;
}

/*
 * test_chunks_address_tests_text_round_trips writes each of 100 addresses
 * as text and parses it back, a prefix of its text naming it, and checks
 * the text of the largest address and two texts that aren't addresses.
 */
void
test_chunks_address_tests_text_round_trips(void)
{
	static const ChunksAddressTestsParsed none = {0};
	ChunksAddressAddress largest;
	StdString text;
	StdString	want;
	ChunksAddressTestsParsed parsed;

	for (int seed = 0; seed < 100; seed++)
	{
		uint8		byte = (uint8) seed;
		ChunksAddressTestsParsed address = {true, chunks_address_Address_of(&byte, 1)};
		size_t		text_len = CHUNKS_ADDRESS_TEXT_LEN;

		text = chunks_address_tests_text(&address.address);

		STD_ASSERT_EQ(266, 13, text.len == text_len, &text.len, std_usize_Debug_fmt, &text_len, std_usize_Debug_fmt);
		parsed = chunks_address_tests_parse(text.ptr, text.len);
		STD_ASSERT_EQ(267, 13, chunks_address_tests_parsed_eq(&parsed, &address), &parsed,
					  chunks_address_tests_parsed_Debug_fmt, &address, chunks_address_tests_parsed_Debug_fmt);
		std_str_index_range_to(text.ptr, text.len, 8, RUST_FILE, 268, 40);
		STD_ASSERT(268, 13, chunks_address_Address_has_prefix(&address.address, text.ptr, 8),
				   "address.has_prefix(&text[..8])");
		std_vec_free_Checked(&text, 1, 1);
	}
	memset(largest.f0, 0xff, sizeof(largest.f0));
	text = chunks_address_tests_text(&largest);

	want = chunks_address_tests_vs('g');
	STD_ASSERT_EQ(270, 9, chunks_address_tests_string_eq(&text, &want), &text, std_String_Debug_fmt, &want,
				  std_String_Debug_fmt);
	std_vec_free_Checked(&text, 1, 1);
	std_vec_free_Checked(&want, 1, 1);
	want = chunks_address_tests_vs('h');
	parsed = chunks_address_tests_parse(want.ptr, want.len);
	STD_ASSERT_EQ(271, 9, chunks_address_tests_parsed_eq(&parsed, &none), &parsed,
				  chunks_address_tests_parsed_Debug_fmt, &none, chunks_address_tests_parsed_Debug_fmt);
	std_vec_free_Checked(&want, 1, 1);
	parsed = chunks_address_tests_parse("0", 1);
	STD_ASSERT_EQ(272, 9, chunks_address_tests_parsed_eq(&parsed, &none), &parsed,
				  chunks_address_tests_parsed_Debug_fmt, &none, chunks_address_tests_parsed_Debug_fmt);
}

/*
 * test_chunks_address_tests_hashes_with_blake3 checks that the address of no
 * bytes is BLAKE3's hash of them, and isn't the empty address.
 */
void
test_chunks_address_tests_hashes_with_blake3(void)
{
	ChunksAddressAddress of = chunks_address_Address_of((const uint8 *) "", 0);
	ChunksAddressAddress hashed;
	blake3_hasher hasher;
	StdString	left;
	StdString	right;

	blake3_hasher_init(&hasher);
	blake3_hasher_finalize(&hasher, hashed.f0, sizeof(hashed.f0));
	left = chunks_address_tests_text(&of);
	right = chunks_address_tests_text(&hashed);
	STD_ASSERT_EQ(277, 9, chunks_address_tests_string_eq(&left, &right), &left, std_String_Debug_fmt, &right,
				  std_String_Debug_fmt);
	std_vec_free_Checked(&left, 1, 1);
	std_vec_free_Checked(&right, 1, 1);
	STD_ASSERT(278, 9, !chunks_address_Address_is_empty(&of), "!Address::of(b\"\").is_empty()");
}

#endif							/* DOLTGRES_TEST_CHUNKS */
