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
 * A storage's free space map's entries: a block's free bytes, its free
 * offsets, and its full flag, kept only while the block has a free offset;
 * the categories heap's free space map rounds free bytes to; and the seek
 * through the base's free space tree, whose summaries are each subtree's
 * largest free bytes.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/lib.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/pin.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/vacuum.h"
#include "port/pg_bitutils.h"
#include "port/pg_bswap.h"
#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/trees/build.h"
#endif

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "free_space.rs"

/* TXN_FREE_SPACE_STEP is the bytes one category spans, heap's FSM_CAT_STEP. */
#define TXN_FREE_SPACE_STEP ((uint64) CHUNKS_BLCKSZ / TXN_FREE_SPACE_CATEGORIES)

/* TXN_FREE_SPACE_CORRUPT is the message of a free space map's entry that doesn't decode. */
static const char TXN_FREE_SPACE_CORRUPT[] = "a free space map's entry is corrupt";

/* txn_free_space_corrupt returns the error of a free space map entry that doesn't decode. */
static HostError
txn_free_space_corrupt(void)
{
	StdString	message;

	std_string_from_Checked(&message, TXN_FREE_SPACE_CORRUPT, sizeof(TXN_FREE_SPACE_CORRUPT) - 1);
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/* txn_free_space_key writes a block's key in a free space map, its four bytes big-endian. */
void
txn_free_space_key(uint32 block, uint8 result[4])
{
	result[0] = (uint8) (block >> 24);
	result[1] = (uint8) (block >> 16);
	result[2] = (uint8) (block >> 8);
	result[3] = (uint8) block;
}

/* txn_free_space_most_len returns the longest value of an entry at a page's most tuples, which a take reserves. */
size_t
txn_free_space_most_len(uint16 most)
{
	return 10 + ((size_t) most + 7) / 8 + 1;
}

/* txn_free_space_set sets an offset's bit in an entry's bitmap, the offset from 1. */
static void
txn_free_space_set(TxnFreeSpaceFree *self, uint16 n, uint32 line, uint32 col)
{
	size_t		at = (size_t) (n - 1) / 8;

	std_index_check(at, TXN_FREE_SPACE_BITMAP_MOST, RUST_FILE, line, col);
	self->offsets[at] |= (uint8) (1 << ((n - 1) % 8));
}

/*
 * txn_free_space_Free_new returns the entry of a block with free bytes and
 * free offsets, each from 1 to a page's most tuples.
 */
TxnFreeSpaceFree
txn_free_space_Free_new(uint64 bytes, const uint16 *free, size_t free_len, uint16 most)
{
	TxnFreeSpaceFree entry = {.bytes = bytes, .len = ((size_t) most + 7) / 8};

	for (size_t i = 0; i < free_len; i++)
		if (free[i] >= 1 && free[i] <= most)
			txn_free_space_set(&entry, free[i], 66, 13);
	return entry;
}

/*
 * txn_free_space_Free_is_clear reports whether the entry has no free offset,
 * which only the fresh end's block's may have.
 */
bool
txn_free_space_Free_is_clear(const TxnFreeSpaceFree *self)
{
	std_range_to_check(self->len, TXN_FREE_SPACE_BITMAP_MOST, RUST_FILE, 112, 9);
	for (size_t i = 0; i < self->len; i++)
		if (self->offsets[i] != 0)
			return false;
	return true;
}

/*
 * txn_free_space_Free_parsed reads an entry as decode does, answering false
 * for every form decode refuses, for a take's step inside a window, which has
 * no error to give.
 */
bool
txn_free_space_Free_parsed(const uint8 *value, size_t value_len, uint32 block, uint64 fresh_end, uint16 most,
						   TxnFreeSpaceFree *result)
{
	size_t		at = 0;
	ChunksVarintMalformed malformed;
	size_t		bitmap_len;

	if (!chunks_varint_decode(value, value_len, &at, &result->bytes, &malformed) || at == value_len)
		return false;
	bitmap_len = value_len - at - 1;
	if (bitmap_len != ((size_t) most + 7) / 8 || bitmap_len > TXN_FREE_SPACE_BITMAP_MOST || value[value_len - 1] > 1)
		return false;
	memset(result->offsets, 0, sizeof(result->offsets));
	if (bitmap_len != 0)
		memcpy(result->offsets, value + at, bitmap_len);
	result->len = bitmap_len;
	result->full = value[value_len - 1] == 1;
	for (size_t bit = most; bit < bitmap_len * 8; bit++)
		if ((result->offsets[bit / 8] & (1 << (bit % 8))) != 0)
			return false;
	return !(txn_free_space_Free_is_clear(result) && (uint64) block != fresh_end >> 16);
}

/*
 * txn_free_space_Free_decode reads the entry of a block at a page's most
 * tuples: its free bytes as a varint, then its bitmap, then its full flag,
 * one byte. A bitmap of another length, one with a bit set past the most
 * tuples, one all clear for any block but the fresh end's of the base, or a
 * flag other than 0 or 1, is data corruption.
 */
bool
txn_free_space_Free_decode(const uint8 *value, size_t value_len, uint32 block, uint64 fresh_end, uint16 most,
						   TxnFreeSpaceFree *result, HostError *error)
{
	if (txn_free_space_Free_parsed(value, value_len, block, fresh_end, most, result))
		return true;
	*error = txn_free_space_corrupt();
	return false;
}

/*
 * txn_free_space_Free_encode_into writes the entry's value in place of what
 * out held, allocating nothing when out has room for most_len's bytes.
 */
void
txn_free_space_Free_encode_into(const TxnFreeSpaceFree *self, StdVec *out)
{
	out->len = 0;
	chunks_varint_put_varint(out, self->bytes);
	std_range_to_check(self->len, TXN_FREE_SPACE_BITMAP_MOST, RUST_FILE, 106, 32);
	std_vec_extend_from_slice_Checked(out, self->offsets, self->len, 1, 1);
	std_vec_u8_push_Checked(out, (uint8) self->full);
}

/* txn_free_space_Free_encode returns the entry's value. */
void
txn_free_space_Free_encode(const TxnFreeSpaceFree *self, StdVec *result)
{
	std_vec_with_capacity_Checked(result, 10 + self->len + 1, 1, 1);
	txn_free_space_Free_encode_into(self, result);
}

/*
 * txn_free_space_Free_lowest returns the block's lowest free offset, which an
 * insert takes, as PageAddItem reuses the lowest unused line pointer, or
 * false when the entry has no free offset.
 */
bool
txn_free_space_Free_lowest(const TxnFreeSpaceFree *self, uint16 *result)
{
	std_range_to_check(self->len, TXN_FREE_SPACE_BITMAP_MOST, RUST_FILE, 118, 20);
	for (size_t byte = 0; byte < self->len; byte++)
		if (self->offsets[byte] != 0)
		{
			*result = (uint16) (byte * 8) + (uint16) pg_rightmost_one_pos32(self->offsets[byte]) + 1;
			return true;
		}
	return false;
}

/* txn_free_space_Free_has reports whether an offset is free in the entry. */
bool
txn_free_space_Free_has(const TxnFreeSpaceFree *self, uint16 offset)
{
	size_t		at = (size_t) STD_SUB(u16, offset, 1, 133, 18);

	return at / 8 < TXN_FREE_SPACE_BITMAP_MOST && (self->offsets[at / 8] & (1 << (at % 8))) != 0;
}

/* txn_free_space_Free_take takes a free offset and its heap bytes from the entry. */
void
txn_free_space_Free_take(TxnFreeSpaceFree *self, uint16 offset, uint64 bytes)
{
	size_t		at = (size_t) STD_SUB(u16, offset, 1, 139, 18);

	std_index_check(at / 8, TXN_FREE_SPACE_BITMAP_MOST, RUST_FILE, 140, 9);
	self->offsets[at / 8] &= (uint8) ~(1 << (at % 8));
	self->bytes = std_saturating_sub_u64(self->bytes, bytes);
}

/*
 * txn_free_space_Free_give_back frees the offsets from first to last and gives
 * back the heap bytes their tuple ids took, as a free of tuple ids does.
 */
void
txn_free_space_Free_give_back(TxnFreeSpaceFree *self, uint16 first, uint16 last, uint64 bytes)
{
	for (uint32 n = Max(first, 1); n <= last; n++)
		txn_free_space_set(self, (uint16) n, 148, 13);
	self->bytes = STD_ADD(u64, self->bytes, bytes, 150, 9);
}

/*
 * txn_free_space_Free_free_from frees every offset from first to a page's
 * most tuples, as the counter's offsets of a fresh page an insert leaves for
 * a new one are handed to the map.
 */
void
txn_free_space_Free_free_from(TxnFreeSpaceFree *self, uint16 first, uint16 most)
{
	for (uint32 n = Max(first, 1); n <= most; n++)
		txn_free_space_set(self, (uint16) n, 157, 13);
}

/*
 * txn_free_space_Free_PartialEq_eq reports whether two entries are equal field
 * by field, their whole bitmaps compared.
 */
bool
txn_free_space_Free_PartialEq_eq(const TxnFreeSpaceFree *self, const TxnFreeSpaceFree *other)
{
	return self->bytes == other->bytes && memcmp(self->offsets, other->offsets, sizeof(self->offsets)) == 0 &&
		self->len == other->len && self->full == other->full;
}

/* txn_free_space_bitmap_Debug_fmt is a [u8; BITMAP_MOST]'s Debug, a list of its bytes. */
static bool
txn_free_space_bitmap_Debug_fmt(const void *value, StdFormatter *f)
{
	StdBytes	bytes = {value, TXN_FREE_SPACE_BITMAP_MOST};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* txn_free_space_Free_Debug_fmt is Free's derived Debug. */
bool
txn_free_space_Free_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnFreeSpaceFree *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Free", 4);

	std_DebugStruct_field(&s, "bytes", 5, &self->bytes, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "offsets", 7, self->offsets, txn_free_space_bitmap_Debug_fmt);
	std_DebugStruct_field(&s, "len", 3, &self->len, std_usize_Debug_fmt);
	std_DebugStruct_field(&s, "full", 4, &self->full, std_bool_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_free_space_free_bytes reads the free bytes an entry's value or a subtree's summary begins with. */
static bool
txn_free_space_free_bytes(const uint8 *value, size_t value_len, uint64 *result, ChunksErrorError *error)
{
	size_t		at = 0;
	ChunksVarintMalformed malformed;

	if (chunks_varint_decode(value, value_len, &at, result, &malformed))
		return true;
	*error = chunks_error_Error_corrupt_RefStr(TXN_FREE_SPACE_CORRUPT, sizeof(TXN_FREE_SPACE_CORRUPT) - 1);
	return false;
}

/*
 * txn_free_space_avail returns a page's free bytes as heap's free space map
 * records them, PageGetHeapFreeSpace's: an entry's free bytes less the line
 * pointer a new row takes.
 */
uint64
txn_free_space_avail(uint64 bytes)
{
	return std_saturating_sub_u64(bytes, TXN_VACUUM_LINE_POINTER);
}

/*
 * txn_free_space_category returns the category of a page's free bytes, as
 * heap's fsm_space_avail_to_cat rounds them down, the highest kept for
 * MAX_REQUEST bytes or more.
 */
uint8
txn_free_space_category(uint64 bytes)
{
	if (bytes >= TXN_FREE_SPACE_MAX_REQUEST)
		return 255;
	return (uint8) Min(bytes / TXN_FREE_SPACE_STEP, 254);
}

/*
 * txn_free_space_needed returns the category a page needs to hold a request
 * of bytes, as heap's fsm_space_needed_to_cat rounds them up, and refuses a
 * request past MAX_REQUEST with heap's error.
 */
bool
txn_free_space_needed(uint64 bytes, uint8 *result, HostError *error)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("invalid FSM request size "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&bytes, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, 2, args};
	StdString	message;

	if (bytes == 0)
		*result = 1;
	else if (bytes > TXN_FREE_SPACE_MAX_REQUEST)
	{
		std_fmt_format_Checked(&message, &arguments);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	else
		*result = (uint8) Min((bytes + TXN_FREE_SPACE_STEP - 1) / TXN_FREE_SPACE_STEP, 255);
	return true;
}

/* txn_free_space_threshold returns the least free bytes a page of a category holds, heap's fsm_space_cat_to_avail. */
uint64
txn_free_space_threshold(uint8 category)
{
	if (category == 255)
		return TXN_FREE_SPACE_MAX_REQUEST;
	return (uint64) category * TXN_FREE_SPACE_STEP;
}

/*
 * txn_free_space_value_bytes returns the free bytes an entry's encoded value
 * holds, read without decoding its offsets.
 */
bool
txn_free_space_value_bytes(const uint8 *value, size_t value_len, uint64 *result, HostError *error)
{
	ChunksErrorError failed;

	if (txn_free_space_free_bytes(value, value_len, result, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

/*
 * txn_free_space_frees reports whether an entry's encoded value holds an
 * offset free, read from its bitmap without decoding the rest.
 */
bool
txn_free_space_frees(const uint8 *value, size_t value_len, uint16 offset, bool *result, HostError *error)
{
	size_t		at = 0;
	uint64		bytes;
	ChunksVarintMalformed malformed;
	size_t		end = std_saturating_sub_usize(value_len, 1);
	size_t		bit;

	if (!chunks_varint_decode(value, value_len, &at, &bytes, &malformed) || at > end || offset == 0)
	{
		*error = txn_free_space_corrupt();
		return false;
	}
	bit = (size_t) offset - 1;
	*result = bit / 8 < end - at && (value[at + bit / 8] & (1 << (bit % 8))) != 0;
	return true;
}

/*
 * txn_free_space_largest is the base's free space tree's summarizer: a
 * subtree's largest free bytes as a varint, from its entries' values at level
 * 0 or its children's summaries above, each of which begins with free bytes.
 */
bool
txn_free_space_largest(uint8 level, const StdBytes *items, size_t items_len, StdVec *result, ChunksErrorError *error)
{
	uint64		most = 0;

	for (size_t i = 0; i < items_len; i++)
	{
		uint64		bytes;

		if (!txn_free_space_free_bytes(items[i].ptr, items[i].len, &bytes, error))
			return false;
		most = Max(most, bytes);
	}
	std_vec_with_capacity_Checked(result, 10, 1, 1);
	chunks_varint_put_varint(result, most);
	return true;
}

/*
 * txn_free_space_shape returns the base's free space tree's shape: its keys in
 * byte order, each subtree summarized by largest.
 */
TreesShape
txn_free_space_shape(void)
{
	TreesShape	shape = {.has_summarizer = true, .order = trees_KeyOrder_Default_default()};

	shape.summarizer.tag = TREES_SUMMARIZER_FN;
	shape.summarizer.u.fn = txn_free_space_largest;
	return shape;
}

/* txn_free_space_seek_captures_t is seek's scope's captures: the tree, the key to start from, and the bytes sought. */
typedef struct txn_free_space_seek_captures_t
{
	TreesStoreNamed tree;
	uint8		from[4];
	uint64		bytes;
} txn_free_space_seek_captures_t;

/* txn_free_space_key_order compares two keys as their bytes order, the shorter first on a tie. */
static int
txn_free_space_key_order(StdBytes a, const uint8 *b, size_t b_len)
{
	int			c = memcmp(a.ptr, b, Min(a.len, b_len));

	if (c != 0)
		return c;
	return a.len < b_len ? -1 : a.len > b_len ? 1 : 0;
}

/*
 * txn_free_space_found is seek's descent through one node, whose entries
 * before from it passes over, an internal node's entry keyed by its child's
 * last key: the lowest block holding at least bytes free, with its value.
 */
static bool
txn_free_space_found(HostIoIo *io, TreesPinPin *pin, TreesStoreNamed tree, const uint8 *from, uint64 bytes,
					 bool *some, uint32 *block, StdVec *value, ChunksErrorError *error)
{
	TreesNodeNode node;

	if (!trees_pin_Pin_node(pin, io, tree, &node, error))
		return false;
	for (size_t i = 0, len = trees_node_Node_len(&node); i < len; i++)
	{
		StdBytes	key = trees_node_Node_key(&node, i);

		if (txn_free_space_key_order(key, from, 4) < 0)
			continue;
		if (trees_node_Node_is_leaf(&node))
		{
			StdBytes	held = trees_node_Node_value(&node, i);
			uint64		free;

			if (!txn_free_space_free_bytes(held.ptr, held.len, &free, error))
				return false;
			if (free >= bytes)
			{
				static const char key_corrupt[] = "a free space map's key is corrupt";
				uint32		be;

				if (key.len != 4)
				{
					*error = chunks_error_Error_corrupt_RefStr(key_corrupt, sizeof(key_corrupt) - 1);
					return false;
				}
				memcpy(&be, key.ptr, 4);
				*block = pg_ntoh32(be);
				std_slice_to_vec_Checked(value, held.ptr, held.len, 1, 1);
				*some = true;
				return true;
			}
		}
		else
		{
			StdBytes	summary = trees_node_Node_summary(&node, i);
			TreesStoreTree child;

			if (summary.ptr != NULL)
			{
				uint64		most;

				if (!txn_free_space_free_bytes(summary.ptr, summary.len, &most, error))
					return false;
				if (most < bytes)
					continue;
			}
			child = trees_node_Node_child_tree(&node, i);
			if (!txn_free_space_found(io, pin, TREES_STORE_TREE_NAMED(&child), from, bytes, some, block, value,
									  error))
				return false;
			if (*some)
				return true;
		}
	}
	*some = false;
	return true;
}

/* txn_free_space_seek_scope is seek's function in its pin's scope. */
static bool
txn_free_space_seek_scope(void *captures, HostIoIo *io, TreesPinPin *pin, bool *some, uint32 *block,
						  StdVec *value, ChunksErrorError *error)
{
	txn_free_space_seek_captures_t *seek = captures;

	return txn_free_space_found(io, pin, seek->tree, seek->from, seek->bytes, some, block, value, error);
}

/* pin's instance at seek's scope, R = Result<Option<(u32, Vec<u8>)>>. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionTupleU32VecU8
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *some, uint32 *block, StdVec *value, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS bool *some; uint32 *block; StdVec *value; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .some = some, .block = block, .value = value, .error = error
#define TREES_PIN_RESULT_ARGS , frame->some, frame->block, frame->value, frame->error
#include "doltgres/trees/pin_template.h"

/*
 * txn_free_space_seek returns the lowest block at or past from whose entry in
 * a base's free space tree holds at least bytes free, with the entry's value,
 * descending only into the subtrees whose summary shows that much.
 */
bool
txn_free_space_seek(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint32 from, uint64 bytes,
					bool *some, uint32 *block, StdVec *value, HostError *error)
{
	txn_free_space_seek_captures_t captures = {.tree = tree, .bytes = bytes};
	ChunksErrorError failed;

	txn_free_space_key(from, captures.from);
	if (trees_pin_pin_ResultOptionTupleU32VecU8(io, store, txn_free_space_seek_scope, &captures, NULL, some, block,
												value, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

#ifdef DOLTGRES_TEST_TXN

/*
 * txn_free_space_tests_refused reports whether a block's value decodes as data
 * corruption, with the fresh end at block 9.
 */
static bool
txn_free_space_tests_refused(const uint8 *value, size_t value_len, uint32 block, uint16 most)
{
	TxnFreeSpaceFree entry;
	HostError	error;
	bool		corrupt;

	if (txn_free_space_Free_decode(value, value_len, block, UINT64CONST(9) << 16 | 1, most, &entry, &error))
		return false;
	corrupt = memcmp(host_Error_sqlstate(&error), "XX001", 5) == 0;
	host_Error_drop(&error);
	return corrupt;
}

/* txn_free_space_tests_decoded returns an entry's decoding, panicking as unwrap does on an error. */
static TxnFreeSpaceFree
txn_free_space_tests_decoded(uint32 line, const StdVec *value, uint32 block, uint64 fresh_end, uint16 most)
{
	TxnFreeSpaceFree entry;
	HostError	error;

	if (!txn_free_space_Free_decode(value->ptr, value->len, block, fresh_end, most, &entry, &error))
		STD_RESULT_EXPECT_FAILED(line, 61, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return entry;
}

/* txn_free_space_tests_assert_eq asserts that two entries are equal. */
static void
txn_free_space_tests_assert_eq(uint32 line, const TxnFreeSpaceFree *left, const TxnFreeSpaceFree *right)
{
	STD_ASSERT_EQ(line, 9, txn_free_space_Free_PartialEq_eq(left, right), left, txn_free_space_Free_Debug_fmt,
				  right, txn_free_space_Free_Debug_fmt);
}

/*
 * test_txn_free_space_tests_a_free_space_entry_reads_back_and_refuses_every_other_form:
 * an entry is its free bytes as a varint and its offsets' bitmap in
 * ceil(most / 8) bytes, offset n at bit (n - 1) % 8 of byte (n - 1) / 8, low
 * bit first; it reads back as it was written; and a bitmap of another
 * length, one with a bit set past the most tuples, and an all-clear one for
 * any block but the fresh end's are data corruption, while the fresh end's
 * all-clear one reads back.
 */
void
test_txn_free_space_tests_a_free_space_entry_reads_back_and_refuses_every_other_form(void)
{
	static const uint16 offsets[] = {1, 9, 291, 0, 292};
	static const uint16 one[] = {1};
	static const uint8 cut[] = {0x80};
	const uint64 fresh_end = UINT64CONST(9) << 16 | 1;
	TxnFreeSpaceFree entry = txn_free_space_Free_new(300, offsets, lengthof(offsets), 291);
	TxnFreeSpaceFree clear = txn_free_space_Free_new(300, NULL, 0, 291);
	TxnFreeSpaceFree full = txn_free_space_Free_new(300, one, 1, 291);
	TxnFreeSpaceFree back;
	StdVec		value;
	StdVec		encoded;
	StdVec		longer;
	uint8		key[4];
	uint8	   *bytes;

	txn_free_space_Free_encode(&entry, &value);
	bytes = value.ptr;
	STD_ASSERT_EQ_U64(281, 9, (uint64) value.len, (uint64) (2 + 37 + 1));
	STD_ASSERT_EQ_U64(282, 9, (uint64) bytes[2 + 37], 0);
	STD_ASSERT(283, 9, bytes[2] == 0x01 && bytes[3] == 0x01, "&value[2..4] == [0b0000_0001, 0b0000_0001]");
	STD_ASSERT_EQ_U64(284, 9, (uint64) bytes[2 + 36], 0x04);
	back = txn_free_space_tests_decoded(285, &value, 4, fresh_end, 291);
	txn_free_space_tests_assert_eq(285, &back, &entry);
	txn_free_space_key(258, key);
	STD_ASSERT(286, 9, key[0] == 0 && key[1] == 0 && key[2] == 1 && key[3] == 2, "key(258) == [0, 0, 1, 2]");
	STD_ASSERT(288, 9, txn_free_space_Free_is_clear(&clear), "clear.is_clear()");
	txn_free_space_Free_encode(&clear, &encoded);
	STD_ASSERT(289, 9, txn_free_space_tests_refused(encoded.ptr, encoded.len, 4, 291), "refused(&clear.encode(), 4, 291)");
	back = txn_free_space_tests_decoded(290, &encoded, 9, fresh_end, 291);
	txn_free_space_tests_assert_eq(290, &back, &clear);
	std_vec_free_Checked(&encoded, 1, 1);
	full.full = true;
	txn_free_space_Free_encode(&full, &encoded);
	STD_ASSERT_EQ_U64(293, 9, (uint64) ((uint8 *) encoded.ptr)[2 + 37], 1);
	back = txn_free_space_tests_decoded(294, &encoded, 4, fresh_end, 291);
	txn_free_space_tests_assert_eq(294, &back, &full);
	std_vec_free_Checked(&encoded, 1, 1);
	std_slice_to_vec_Checked(&encoded, value.ptr, value.len, 1, 1);
	((uint8 *) encoded.ptr)[2 + 37] = 2;
	STD_ASSERT(297, 9, txn_free_space_tests_refused(encoded.ptr, encoded.len, 4, 291), "refused(&flagged, 4, 291)");
	((uint8 *) encoded.ptr)[2 + 37] = 0;
	((uint8 *) encoded.ptr)[2 + 36] |= 0x08;
	STD_ASSERT(300, 9, txn_free_space_tests_refused(encoded.ptr, encoded.len, 4, 291), "refused(&past, 4, 291)");
	STD_ASSERT(301, 9, txn_free_space_tests_refused(encoded.ptr, encoded.len, 9, 291), "refused(&past, 9, 291)");
	std_vec_free_Checked(&encoded, 1, 1);
	STD_ASSERT(302, 9, txn_free_space_tests_refused(value.ptr, value.len - 1, 4, 291),
			   "refused(&value[..value.len() - 1], 4, 291)");
	std_slice_to_vec_Checked(&longer, value.ptr, value.len, 1, 1);
	std_vec_u8_push_Checked(&longer, 0);
	STD_ASSERT(303, 9, txn_free_space_tests_refused(longer.ptr, longer.len, 4, 291),
			   "refused(&[value.as_slice(), &[0]].concat(), 4, 291)");
	std_vec_free_Checked(&longer, 1, 1);
	STD_ASSERT(304, 9, txn_free_space_tests_refused(cut, 1, 9, 291), "refused(&[0x80], 9, 291)");
	STD_ASSERT(305, 9, txn_free_space_tests_refused(value.ptr, value.len, 4, 584), "refused(&value, 4, 584)");
	std_vec_free_Checked(&value, 1, 1);
}

/*
 * test_txn_free_space_tests_an_insert_takes_the_lowest_free_offset_and_a_left_fresh_page_frees_its_counter_offsets:
 * an insert takes a block's lowest free offset and its bytes, as PageAddItem
 * reuses the lowest unused line pointer, and a fresh page left for a new one
 * hands its counter's offsets to the map.
 */
void
test_txn_free_space_tests_an_insert_takes_the_lowest_free_offset_and_a_left_fresh_page_frees_its_counter_offsets(void)
{
	static const uint16 offsets[] = {12, 3, 200};
	static const uint16 seven[] = {7};
	uint16		freed[1 + 91];
	TxnFreeSpaceFree entry = txn_free_space_Free_new(500, offsets, lengthof(offsets), 291);
	TxnFreeSpaceFree left = txn_free_space_Free_new(40, seven, 1, 291);
	TxnFreeSpaceFree expected;
	uint16		lowest = 0;
	bool		some;

	some = txn_free_space_Free_lowest(&entry, &lowest);
	STD_ASSERT(313, 9, some && lowest == 3, "entry.lowest() == Some(3)");
	txn_free_space_Free_take(&entry, 3, 100);
	some = txn_free_space_Free_lowest(&entry, &lowest);
	STD_ASSERT(315, 9, some && lowest == 12 && entry.bytes == 400, "(entry.lowest(), entry.bytes) == (Some(12), 400)");
	txn_free_space_Free_take(&entry, 12, 100);
	txn_free_space_Free_take(&entry, 200, 300);
	some = txn_free_space_Free_lowest(&entry, &lowest);
	STD_ASSERT(318, 9, !some && entry.bytes == 0 && txn_free_space_Free_is_clear(&entry),
			   "(entry.lowest(), entry.bytes, entry.is_clear()) == (None, 0, true)");
	txn_free_space_Free_free_from(&left, 201, 291);
	freed[0] = 7;
	for (uint16 n = 201; n <= 291; n++)
		freed[n - 200] = n;
	expected = txn_free_space_Free_new(40, freed, lengthof(freed), 291);
	txn_free_space_tests_assert_eq(321, &left, &expected);
}

/* TXN_FREE_SPACE_TESTS_UNWRAP is unwrap's panic message on an error. */
#define TXN_FREE_SPACE_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* txn_free_space_tests_sought reports whether a seek from a block finds one with room, and which. */
static bool
txn_free_space_tests_sought(HostIoIo *io, TreesStoreMemoryStore *store, TreesStoreTreeRef *tree, uint32 from,
							uint64 bytes, uint32 *block)
{
	bool		some;
	StdVec		value;
	HostError	error;

	if (!txn_free_space_seek(io, TREES_STORE_MEMORY_STORE_NODE_STORE(store), TREES_STORE_TREE_REF_NAMED(tree), from,
							 bytes, &some, block, &value, &error))
		STD_RESULT_EXPECT_FAILED(343, 59, TXN_FREE_SPACE_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	if (some)
		std_vec_free_Checked(&value, 1, 1);
	return some;
}

/*
 * test_txn_free_space_tests_a_seek_finds_the_lowest_block_with_room_through_the_summaries:
 * in a large base map whose room is only at high blocks, the seek gives the
 * lowest block at or past its start with room for the row, through each
 * subtree's summary of its largest free bytes, and none past the last.
 */
void
test_txn_free_space_tests_a_seek_finds_the_lowest_block_with_room_through_the_summaries(void)
{
	static const uint16 one[] = {1};
	static const struct
	{
		uint32		from;
		uint64		bytes;
		bool		some;
		uint32		block;
	}			cases[] = {{0, 500, true, 1500}, {1501, 500, true, 1900}, {1500, 700, true, 1900},
	{1901, 500, false, 0}, {7, 5, true, 7}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	TreesShape	shape = txn_free_space_shape();
	TreesBuildBuilder builder;
	TreesStoreFresh fresh;
	TreesStoreTreeRef tree;
	ChunksErrorError failed;
	bool		some;
	uint32		block;
	StdVec		value;
	HostError	error;
	TxnFreeSpaceFree back;
	TxnFreeSpaceFree expected;

	trees_build_Builder_shaped(TREES_STORE_MEMORY_STORE_NODE_STORE(&store), 256, &shape, &builder);
	for (uint32 n = 0; n < 2000; n++)
	{
		TxnFreeSpaceFree entry = txn_free_space_Free_new(n == 1500 ? 600 : n == 1900 ? 800 : 10, one, 1, 291);
		TreesBuildEntry built;
		uint8		key[4];

		txn_free_space_key(n, key);
		std_slice_to_vec_Checked(&built.key, key, 4, 1, 1);
		txn_free_space_Free_encode(&entry, &built.value);
		std_vec_new(&built.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		std_vec_new(&built.stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		std_vec_new(&built.values, sizeof(uint32), _Alignof(uint32));
		if (!trees_build_Builder_add(&builder, io, &built, &failed))
			STD_RESULT_EXPECT_FAILED(338, 99, TXN_FREE_SPACE_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	}
	if (!trees_build_Builder_finish(&builder, io, &TREES_STORE_MEMORY_STORE_BRANDED_OPS, &fresh, &failed))
		STD_RESULT_EXPECT_FAILED(340, 34, TXN_FREE_SPACE_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	tree = trees_store_Fresh_as_ref(&fresh);
	for (size_t i = 0; i < lengthof(cases); i++)
	{
		bool		found = txn_free_space_tests_sought(io, &store, &tree, cases[i].from, cases[i].bytes, &block);

		STD_ASSERT(344, 9, found == cases[i].some && (!found || block == cases[i].block),
				   "sought(io, from, bytes) == the block with room");
	}
	if (!txn_free_space_seek(io, TREES_STORE_MEMORY_STORE_NODE_STORE(&store), TREES_STORE_TREE_REF_NAMED(&tree), 0,
							 700, &some, &block, &value, &error))
		STD_RESULT_EXPECT_FAILED(349, 51, TXN_FREE_SPACE_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	if (!some)
		STD_OPTION_UNWRAP_FAILED(349, 60);
	back = txn_free_space_tests_decoded(350, &value, 1900, 1, 291);
	expected = txn_free_space_Free_new(800, one, 1, 291);
	txn_free_space_tests_assert_eq(350, &back, &expected);
	std_vec_free_Checked(&value, 1, 1);
	trees_store_MemoryStore_drop(&store);
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_encode: an
 * entry of more tuples than a bitmap holds panics where encode slices its
 * bitmap, as the Rust's slice does.
 */
void
test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_encode(void)
{
	TxnFreeSpaceFree entry = txn_free_space_Free_new(300, NULL, 0, 2000);
	StdVec		value;

	std_vec_new(&value, 1, 1);
	txn_free_space_Free_encode_into(&entry, &value);
}

/*
 * test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_is_clear: an
 * entry of more tuples than a bitmap holds panics where is_clear slices its
 * bitmap.
 */
void
test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_is_clear(void)
{
	TxnFreeSpaceFree entry = txn_free_space_Free_new(300, NULL, 0, 2000);

	(void) txn_free_space_Free_is_clear(&entry);
}

/*
 * test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_lowest: an
 * entry of more tuples than a bitmap holds panics where lowest slices its
 * bitmap.
 */
void
test_txn_free_space_tests_an_entry_past_the_bitmap_panics_at_lowest(void)
{
	TxnFreeSpaceFree entry = txn_free_space_Free_new(300, NULL, 0, 2000);
	uint16		lowest;

	(void) txn_free_space_Free_lowest(&entry, &lowest);
}

#endif							/* DOLTGRES_TEST_TXN */
