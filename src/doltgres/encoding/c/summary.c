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
 * The summaries a summarized tree's internal nodes keep of each child's
 * subtree, as a GiST index keeps the union of a subtree's keys: each kind's
 * bytes and the function that makes them from a node's leaf values or its
 * children's summaries.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <math.h>

#include "doltgres/chunks/varint.h"
#include "doltgres/encoding/key.h"
#include "doltgres/encoding/summary.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE "encoding" STD_SEP "src" STD_SEP "summary.rs"

static ChunksErrorError encoding_summary_corrupt(void);
static bool encoding_summary_gist_union(uint8 level, const StdBytes *items, size_t items_len,
										StdVec *result, ChunksErrorError *error);
static bool encoding_summary_gist_counted_union(uint8 level, const StdBytes *items,
												size_t items_len, StdVec *result,
												ChunksErrorError *error);
static bool encoding_summary_box_union(const encoding_summary_gist_keys_t *keys, size_t keys_len,
									   StdVec *result, ChunksErrorError *error);
static bool encoding_summary_inet_union(const encoding_summary_gist_keys_t *keys, size_t keys_len,
										StdVec *result, ChunksErrorError *error);
static bool encoding_summary_range_union(const encoding_summary_gist_keys_t *keys, size_t keys_len,
										 StdVec *result, ChunksErrorError *error);
static bool encoding_summary_tsvector_key_len(const uint8 *bytes, size_t bytes_len, size_t *result,
											  ChunksErrorError *error);
static bool encoding_summary_tsvector_union(const encoding_summary_gist_keys_t *keys, size_t keys_len,
											StdVec *result, ChunksErrorError *error);
static bool encoding_summary_tsquery_union(const encoding_summary_gist_keys_t *keys, size_t keys_len,
										   StdVec *result, ChunksErrorError *error);

/*
 * encoding_summary_summarizer returns the function that makes a kind's
 * summaries of a subtree at a level from its leaf values or its children's
 * summaries, None for kind 0, which keeps none, and for the kinds whose
 * summaries the engine makes, and an error for a kind this version does not
 * know.
 */
bool
encoding_summary_summarizer(uint8 kind, EncodingSummarySummarizer *result, ChunksErrorError *error)
{
	switch (kind)
	{
		case 0:
		case ENCODING_SUMMARY_GIST_COMPARED:
		case ENCODING_SUMMARY_GIST_COMPARED_COUNTED:
			*result = NULL;
			return true;
		case ENCODING_SUMMARY_GIST:
			*result = encoding_summary_gist_union;
			return true;
		case ENCODING_SUMMARY_GIST_COUNTED:
			*result = encoding_summary_gist_counted_union;
			return true;
		default:
			{
				StdString	message;
				StdFmtPiece pieces[] = {STD_FMT_LIT("unknown tree summary kind "), STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{&kind, std_u8_Display_fmt}};
				StdFmtArguments arguments = {pieces, 2, args};

				std_fmt_format_Checked(&message, &arguments);
				*error = chunks_error_Error_corrupt(&message);
				return false;
			}
	}
}

/* encoding_summary_engine_made reports whether the engine makes a summary kind's summaries. */
bool
encoding_summary_engine_made(uint8 kind)
{
	return kind == ENCODING_SUMMARY_GIST_COMPARED || kind == ENCODING_SUMMARY_GIST_COMPARED_COUNTED;
}

/* encoding_summary_corrupt returns the error for a GiST value whose keys cannot be read. */
static ChunksErrorError
encoding_summary_corrupt(void)
{
	StdString	message;

	std_string_from_Checked(&message, "a GiST index value's keys are corrupt", 37);
	return chunks_error_Error_corrupt(&message);
}

/*
 * encoding_summary_gist_keys returns the tags and key bytes of a GiST leaf
 * value's or summary's columns, and the bytes after them.
 */
bool
encoding_summary_gist_keys(const uint8 *value, size_t value_len, StdVec *result_0, StdBytes *result_1,
						   ChunksErrorError *error)
{
	uint8		count;
	StdBytes	rest;
	volatile StdVec keys;
	volatile bool keys_live = false;
	StdUnwind	unwind;

	if (value_len == 0)
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	count = value[0];
	rest.ptr = value + 1;
	rest.len = value_len - 1;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_with_capacity_Checked((StdVec *) &keys, (size_t) count, sizeof(encoding_summary_gist_keys_t),
									  _Alignof(encoding_summary_gist_keys_t));
		keys_live = true;
		for (uint8 i = 0, i_end = count; i < i_end; i++)
		{
			uint8		tag;
			StdBytes	after;
			size_t		size;
			encoding_summary_gist_keys_t item;

			if (rest.len == 0)
			{
				*error = encoding_summary_corrupt();
				keys_live = false;
				std_vec_free_Checked((StdVec *) &keys, sizeof(encoding_summary_gist_keys_t),
									 _Alignof(encoding_summary_gist_keys_t));
				std_unwind_pop(&unwind);
				return false;
			}
			tag = rest.ptr[0];
			after.ptr = rest.ptr + 1;
			after.len = rest.len - 1;
			switch (tag)
			{
				case ENCODING_SUMMARY_GIST_NULL:
					size = 0;
					break;
				case ENCODING_SUMMARY_GIST_BOX:
					size = 32;
					break;
				case ENCODING_SUMMARY_GIST_POINT:
					size = 16;
					break;
				case ENCODING_SUMMARY_GIST_INET:
					size = STD_ADD(usize, 3, after.len > 0 && after.ptr[0] == ENCODING_SUMMARY_PGSQL_AF_INET6 ? 16 : 4,
								   127, 26);
					break;
				case ENCODING_SUMMARY_GIST_RANGE:
				case ENCODING_SUMMARY_GIST_RANGE_DATUM:
					{
						EncodingSummaryRangeKey range;

						if (!encoding_summary_RangeKey_read(after.ptr, after.len, &range, &size, error))
						{
							keys_live = false;
							std_vec_free_Checked((StdVec *) &keys, sizeof(encoding_summary_gist_keys_t),
												 _Alignof(encoding_summary_gist_keys_t));
							std_unwind_pop(&unwind);
							return false;
						}
					}
					break;
				case ENCODING_SUMMARY_GIST_TSVECTOR:
					if (!encoding_summary_tsvector_key_len(after.ptr, after.len, &size, error))
					{
						keys_live = false;
						std_vec_free_Checked((StdVec *) &keys, sizeof(encoding_summary_gist_keys_t),
											 _Alignof(encoding_summary_gist_keys_t));
						std_unwind_pop(&unwind);
						return false;
					}
					break;
				case ENCODING_SUMMARY_GIST_TSQUERY:
					size = 8;
					break;
				default:
					*error = encoding_summary_corrupt();
					keys_live = false;
					std_vec_free_Checked((StdVec *) &keys, sizeof(encoding_summary_gist_keys_t),
										 _Alignof(encoding_summary_gist_keys_t));
					std_unwind_pop(&unwind);
					return false;
			}
			if (after.len < size)
			{
				*error = encoding_summary_corrupt();
				keys_live = false;
				std_vec_free_Checked((StdVec *) &keys, sizeof(encoding_summary_gist_keys_t),
									 _Alignof(encoding_summary_gist_keys_t));
				std_unwind_pop(&unwind);
				return false;
			}
			std_range_to_check(size, after.len, RUST_FILE, 136, 31);
			item.f0 = tag;
			item.f1.ptr = after.ptr;
			item.f1.len = size;
			std_vec_push_Checked((StdVec *) &keys, &item, sizeof(encoding_summary_gist_keys_t),
								 _Alignof(encoding_summary_gist_keys_t));
			std_range_from_check(size, after.len, RUST_FILE, 137, 22);
			rest.ptr = after.ptr + size;
			rest.len = after.len - size;
		}
		*result_0 = *(StdVec *) &keys;
		keys_live = false;
		*result_1 = rest;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (keys_live)
			std_vec_free_Checked((StdVec *) &keys, sizeof(encoding_summary_gist_keys_t),
								 _Alignof(encoding_summary_gist_keys_t));
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * encoding_summary_VecTupleU8RefSliceU8_drop is the drop glue of a
 * Vec<(u8, &[u8])>, a row of a GiST node's column keys.
 */
static void
encoding_summary_VecTupleU8RefSliceU8_drop(void *self)
{
	std_vec_free_Checked((StdVec *) self, sizeof(encoding_summary_gist_keys_t),
						 _Alignof(encoding_summary_gist_keys_t));
}

/*
 * encoding_summary_gist_union returns the union of each column of a GiST
 * node's leaf values or children's summaries.
 */
static bool
encoding_summary_gist_union(uint8 level, const StdBytes *items, size_t items_len, StdVec *result,
							ChunksErrorError *error)
{
	volatile StdVec rows;
	volatile bool rows_live = false;
	volatile StdVec out;
	volatile bool out_live = false;
	volatile StdVec keys;
	volatile bool keys_live = false;
	StdUnwind	unwind;

	if (!encoding_summary_gist_rows(items, items_len, (StdVec *) &rows, error))
		return false;
	rows_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *rows_v = (StdVec *) &rows;
		StdVec	   *out_v = (StdVec *) &out;
		StdVec	   *keys_v = (StdVec *) &keys;
		uint8		first = (uint8) (rows_v->len > 0 ? STD_VEC_AT(rows_v, StdVec, 0).len : 0);
		void	   *boxed = std_box_new_uninit_layout_Checked(1, 1);

		STD_CHECK_ALIGNED(boxed, 1, 145, 19);
		STD_CHECK_NULL_DEREF(boxed, 145, 19);
		*(uint8 *) boxed = first;
		std_box_slice_into_vec_Checked(out_v, boxed, 1, 1);
		out_live = true;
		std_index_check(0, out_v->len, RUST_FILE, 146, 25);
		for (size_t column = 0, column_end = (size_t) STD_VEC_AT(out_v, uint8, 0); column < column_end; column++)
		{
			for (size_t r = 0, r_len = rows_v->len; r < r_len; r++)
			{
				const StdVec *row = &STD_VEC_AT(rows_v, StdVec, r);
				encoding_summary_gist_keys_t item;

				std_index_check(column, row->len, RUST_FILE, 147, 63);
				item = STD_VEC_AT(row, encoding_summary_gist_keys_t, column);
				if (item.f0 == ENCODING_SUMMARY_GIST_NULL)
					continue;
				if (!keys_live)
				{
					std_vec_collect_first(keys_v, &item, 0, sizeof(encoding_summary_gist_keys_t),
										  _Alignof(encoding_summary_gist_keys_t));
					keys_live = true;
				}
				else
					std_vec_extend_one(keys_v, &item, 0, sizeof(encoding_summary_gist_keys_t),
									   _Alignof(encoding_summary_gist_keys_t));
			}
			if (!keys_live)
			{
				std_vec_new(keys_v, sizeof(encoding_summary_gist_keys_t), _Alignof(encoding_summary_gist_keys_t));
				keys_live = true;
			}
			if (!encoding_summary_column_union(out_v, keys_v->ptr, keys_v->len, error))
			{
				keys_live = false;
				std_vec_free_Checked(keys_v, sizeof(encoding_summary_gist_keys_t),
									 _Alignof(encoding_summary_gist_keys_t));
				out_live = false;
				std_vec_free_Checked(out_v, 1, 1);
				rows_live = false;
				std_vec_drop_Checked(rows_v, sizeof(StdVec), _Alignof(StdVec),
									 encoding_summary_VecTupleU8RefSliceU8_drop);
				std_unwind_pop(&unwind);
				return false;
			}
			keys_live = false;
			std_vec_free_Checked(keys_v, sizeof(encoding_summary_gist_keys_t),
								 _Alignof(encoding_summary_gist_keys_t));
		}
		*result = *out_v;
		out_live = false;
		rows_live = false;
		std_vec_drop_Checked(rows_v, sizeof(StdVec), _Alignof(StdVec), encoding_summary_VecTupleU8RefSliceU8_drop);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (keys_live)
			std_vec_free_Checked((StdVec *) &keys, sizeof(encoding_summary_gist_keys_t),
								 _Alignof(encoding_summary_gist_keys_t));
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		if (rows_live)
			std_vec_drop_Checked((StdVec *) &rows, sizeof(StdVec), _Alignof(StdVec),
								 encoding_summary_VecTupleU8RefSliceU8_drop);
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * encoding_summary_gist_rows returns the column keys of each of a GiST node's
 * leaf values or children's summaries, failing unless they all have the same
 * number of columns.
 */
bool
encoding_summary_gist_rows(const StdBytes *items, size_t items_len, StdVec *result, ChunksErrorError *error)
{
	volatile StdVec rows;
	volatile bool rows_live = false;
	volatile StdVec tmp_1;
	volatile bool tmp_1_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *rows_v = (StdVec *) &rows;
		size_t		count;

		std_vec_with_capacity_Checked(rows_v, items_len, sizeof(StdVec), _Alignof(StdVec));
		rows_live = true;
		for (size_t i = 0, i_len = items_len; i < i_len; i++)
		{
			const StdBytes *item = &items[i];
			StdBytes	ignored;

			if (!encoding_summary_gist_keys(item->ptr, item->len, (StdVec *) &tmp_1, &ignored, error))
			{
				rows_live = false;
				std_vec_drop_Checked(rows_v, sizeof(StdVec), _Alignof(StdVec),
									 encoding_summary_VecTupleU8RefSliceU8_drop);
				std_unwind_pop(&unwind);
				return false;
			}
			tmp_1_live = true;
			std_vec_push_Checked(rows_v, (StdVec *) &tmp_1, sizeof(StdVec), _Alignof(StdVec));
			tmp_1_live = false;
		}
		count = rows_v->len > 0 ? STD_VEC_AT(rows_v, StdVec, 0).len : 0;
		for (size_t i = 0, i_len = rows_v->len; i < i_len; i++)
		{
			if (STD_VEC_AT(rows_v, StdVec, i).len != count)
			{
				*error = encoding_summary_corrupt();
				rows_live = false;
				std_vec_drop_Checked(rows_v, sizeof(StdVec), _Alignof(StdVec),
									 encoding_summary_VecTupleU8RefSliceU8_drop);
				std_unwind_pop(&unwind);
				return false;
			}
		}
		*result = *rows_v;
		rows_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			encoding_summary_VecTupleU8RefSliceU8_drop((StdVec *) &tmp_1);
		if (rows_live)
			std_vec_drop_Checked((StdVec *) &rows, sizeof(StdVec), _Alignof(StdVec),
								 encoding_summary_VecTupleU8RefSliceU8_drop);
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * encoding_summary_column_union appends the tag and bytes of the union of a
 * column's keys other than NULL, or a NULL key when there are none.
 */
bool
encoding_summary_column_union(StdVec *out, const encoding_summary_gist_keys_t *keys, size_t keys_len,
							  ChunksErrorError *error)
{
	bool		has_tag = keys_len > 0;
	uint8		tag = has_tag ? keys[0].f0 : 0;
	volatile StdVec tmp_1;
	volatile bool tmp_1_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *tmp_1_v = (StdVec *) &tmp_1;
		bool		ok;

		if (!has_tag)
		{
			std_vec_u8_push_Checked(out, ENCODING_SUMMARY_GIST_NULL);
			std_unwind_pop(&unwind);
			return true;
		}
		else if (tag == ENCODING_SUMMARY_GIST_INET)
		{
			std_vec_u8_push_Checked(out, ENCODING_SUMMARY_GIST_INET);
			ok = encoding_summary_inet_union(keys, keys_len, tmp_1_v, error);
		}
		else if (tag == ENCODING_SUMMARY_GIST_RANGE)
		{
			std_vec_u8_push_Checked(out, ENCODING_SUMMARY_GIST_RANGE);
			ok = encoding_summary_range_union(keys, keys_len, tmp_1_v, error);
		}
		else if (tag == ENCODING_SUMMARY_GIST_TSVECTOR)
		{
			std_vec_u8_push_Checked(out, ENCODING_SUMMARY_GIST_TSVECTOR);
			ok = encoding_summary_tsvector_union(keys, keys_len, tmp_1_v, error);
		}
		else if (tag == ENCODING_SUMMARY_GIST_TSQUERY)
		{
			std_vec_u8_push_Checked(out, ENCODING_SUMMARY_GIST_TSQUERY);
			ok = encoding_summary_tsquery_union(keys, keys_len, tmp_1_v, error);
		}
		else
		{
			std_vec_u8_push_Checked(out, ENCODING_SUMMARY_GIST_BOX);
			ok = encoding_summary_box_union(keys, keys_len, tmp_1_v, error);
		}
		if (!ok)
		{
			std_unwind_pop(&unwind);
			return false;
		}
		tmp_1_live = true;
		std_vec_extend_from_slice_Checked(out, tmp_1_v->ptr, tmp_1_v->len, 1, 1);
		tmp_1_live = false;
		std_vec_free_Checked(tmp_1_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			std_vec_free_Checked((StdVec *) &tmp_1, 1, 1);
		std_unwind_resume(&unwind);
	}
	return true;
}

/* encoding_summary_gist_counted_union is gist_union over leaf values that start with a count of copies. */
static bool
encoding_summary_gist_counted_union(uint8 level, const StdBytes *items, size_t items_len, StdVec *result,
									ChunksErrorError *error)
{
	switch (level)
	{
		case 0:
			{
				volatile StdVec tmp_1;
				volatile bool tmp_1_live = false;
				bool		ok;
				StdUnwind	unwind;

				if (!encoding_summary_uncounted(items, items_len, (StdVec *) &tmp_1, error))
					return false;
				tmp_1_live = true;
				STD_UNWIND_PUSH(&unwind);
				if (sigsetjmp(unwind.jump, 0) == 0)
				{
					StdVec	   *tmp_1_v = (StdVec *) &tmp_1;

					ok = encoding_summary_gist_union(level, tmp_1_v->ptr, tmp_1_v->len, result, error);
					tmp_1_live = false;
					std_vec_free_Checked(tmp_1_v, sizeof(StdBytes), _Alignof(StdBytes));
					std_unwind_pop(&unwind);
				}
				else
				{
					if (tmp_1_live)
						std_vec_free_Checked((StdVec *) &tmp_1, sizeof(StdBytes), _Alignof(StdBytes));
					std_unwind_resume(&unwind);
				}
				return ok;
			}
		default:
			return encoding_summary_gist_union(level, items, items_len, result, error);
	}
}

/*
 * encoding_summary_uncounted returns leaf values that start with a count of
 * copies without their counts.
 */
bool
encoding_summary_uncounted(const StdBytes *items, size_t items_len, StdVec *result, ChunksErrorError *error)
{
	volatile StdVec values;
	volatile bool values_live = false;
	volatile ChunksErrorOwn own;
	volatile bool own_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *values_v = (StdVec *) &values;

		std_vec_with_capacity_Checked(values_v, items_len, sizeof(StdBytes), _Alignof(StdBytes));
		values_live = true;
		for (size_t i = 0, i_len = items_len; i < i_len; i++)
		{
			const StdBytes *item = &items[i];
			size_t		at = 0;
			uint64		ignored;
			ChunksErrorError failure;
			StdBytes	value;

			if (!chunks_varint_read_varint(item->ptr, item->len, &at, &ignored, &failure))
			{
				ChunksErrorError other;

				if (chunks_error_Error_own(&failure, (ChunksErrorOwn *) &own, &other))
				{
					own_live = true;
					*error = encoding_summary_corrupt();
					own_live = false;
					chunks_error_Own_drop((ChunksErrorOwn *) &own);
				}
				else
					*error = other;
				values_live = false;
				std_vec_free_Checked(values_v, sizeof(StdBytes), _Alignof(StdBytes));
				std_unwind_pop(&unwind);
				return false;
			}
			std_range_from_check(at, item->len, RUST_FILE, 214, 26);
			value.ptr = item->ptr + at;
			value.len = item->len - at;
			std_vec_push_Checked(values_v, &value, sizeof(StdBytes), _Alignof(StdBytes));
		}
		*result = *values_v;
		values_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (own_live)
			chunks_error_Own_drop((ChunksErrorOwn *) &own);
		if (values_live)
			std_vec_free_Checked((StdVec *) &values, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_resume(&unwind);
	}
	return true;
}

/* encoding_summary_double reads the little-endian double at an offset. */
static float8
encoding_summary_double(const uint8 *bytes, size_t bytes_len, size_t at)
{
	size_t		end = STD_ADD(usize, at, 8, 221, 34);

	std_range_check(at, end, bytes_len, RUST_FILE, 221, 29);
	if (end - at != 8)
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt, RUST_FILE, 221, 53);
	return std_f64_from_le_bytes(bytes + at);
}

/* encoding_summary_float8_lt is < with NaN above every other value, Postgres' float8_lt. */
static bool
encoding_summary_float8_lt(float8 a, float8 b)
{
	return a < b || (!std_f64_is_nan(a) && std_f64_is_nan(b));
}

/* encoding_summary_float8_gt is > with NaN above every other value, Postgres' float8_gt. */
static bool
encoding_summary_float8_gt(float8 a, float8 b)
{
	return a > b || (std_f64_is_nan(a) && !std_f64_is_nan(b));
}

/*
 * encoding_summary_adjust_box widens a box, as high x, high y, low x, and low
 * y, to cover another, Postgres' adjustBox.
 */
static void
encoding_summary_adjust_box(float8 *b, const float8 *addon)
{
	if (encoding_summary_float8_lt(b[0], addon[0]))
		b[0] = addon[0];
	if (encoding_summary_float8_gt(b[2], addon[2]))
		b[2] = addon[2];
	if (encoding_summary_float8_lt(b[1], addon[1]))
		b[1] = addon[1];
	if (encoding_summary_float8_gt(b[3], addon[3]))
		b[3] = addon[3];
}

/*
 * encoding_summary_box_union returns the box covering box and point keys,
 * starting from the first, as Postgres' gist_box_union does over the boxes
 * gist_point_compress makes of points, except that a key with a NaN
 * coordinate widens the union to the whole of that axis, since a consistent
 * function's tests of a NaN bound fail where its key's would pass.
 */
static bool
encoding_summary_box_union(const encoding_summary_gist_keys_t *keys, size_t keys_len, StdVec *result,
						   ChunksErrorError *error)
{
	bool		has_union = false;
	float8		union_[4];
	float8		values[4] = {0};
	volatile StdVec out;
	volatile bool out_live = false;
	StdUnwind	unwind;

	for (size_t k = 0, k_len = keys_len; k < k_len; k++)
	{
		uint8		tag = keys[k].f0;
		StdBytes	key = keys[k].f1;
		float8		b[4];

		switch (tag)
		{
			case ENCODING_SUMMARY_GIST_BOX:
				b[0] = encoding_summary_double(key.ptr, key.len, 0);
				b[1] = encoding_summary_double(key.ptr, key.len, 8);
				b[2] = encoding_summary_double(key.ptr, key.len, 16);
				b[3] = encoding_summary_double(key.ptr, key.len, 24);
				break;
			case ENCODING_SUMMARY_GIST_POINT:
				b[0] = encoding_summary_double(key.ptr, key.len, 0);
				b[1] = encoding_summary_double(key.ptr, key.len, 8);
				b[2] = encoding_summary_double(key.ptr, key.len, 0);
				b[3] = encoding_summary_double(key.ptr, key.len, 8);
				break;
			default:
				*error = encoding_summary_corrupt();
				return false;
		}
		for (size_t axis = 0, axis_end = 2; axis < axis_end; axis++)
		{
			size_t		tmp_1;
			bool		nan;

			std_index_check(axis, 4, RUST_FILE, 262, 16);
			nan = std_f64_is_nan(b[axis]);
			if (!nan)
			{
				tmp_1 = STD_ADD(usize, axis, 2, 262, 38);
				std_index_check(tmp_1, 4, RUST_FILE, 262, 36);
				nan = std_f64_is_nan(b[tmp_1]);
			}
			if (nan)
			{
				std_index_check(axis, 4, RUST_FILE, 263, 18);
				b[axis] = INFINITY;
				tmp_1 = STD_ADD(usize, axis, 2, 263, 29);
				std_index_check(tmp_1, 4, RUST_FILE, 263, 27);
				b[tmp_1] = -INFINITY;
			}
		}
		if (!has_union)
		{
			memcpy(union_, b, sizeof(union_));
			has_union = true;
		}
		else
			encoding_summary_adjust_box(union_, b);
	}
	if (has_union)
		memcpy(values, union_, sizeof(values));
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;
		StdSizeHint hint = {32, true, 32};

		std_vec_with_capacity_trusted(out_v, hint, 1, 1);
		out_live = true;
		std_vec_extend_trusted_reserve(out_v, hint, 1, 1);
		for (size_t v = 0, v_len = 4; v < v_len; v++)
		{
			uint8		bytes[8];

			std_f64_to_le_bytes(values[v], bytes);
			for (size_t i = 0, i_len = 8; i < i_len; i++)
			{
				STD_VEC_AT(out_v, uint8, out_v->len) = bytes[i];
				out_v->len++;
			}
		}
		*result = *out_v;
		out_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * encoding_summary_bitncommon returns how many of the first n bits of two
 * addresses match before the first that differs.
 */
size_t
encoding_summary_bitncommon(const uint8 *l, size_t l_len, const uint8 *r, size_t r_len, size_t n)
{
	size_t		nbits = STD_REM_FOLDED(usize, n, 8);
	size_t		byte = 0;
	size_t		tmp_1;

	while (byte < STD_DIV_FOLDED(usize, n, 8))
	{
		std_index_check(byte, l_len, RUST_FILE, 279, 12);
		std_index_check(byte, r_len, RUST_FILE, 279, 23);
		if (l[byte] != r[byte])
		{
			nbits = 7;
			break;
		}
		byte = STD_ADD(usize, byte, 1, 283, 9);
	}
	if (nbits != 0)
	{
		uint32		diff;

		std_index_check(byte, l_len, RUST_FILE, 286, 21);
		std_index_check(byte, r_len, RUST_FILE, 286, 31);
		diff = (uint32) (uint8) (l[byte] ^ r[byte]);
		for (;;)
		{
			tmp_1 = STD_SUB(usize, 8, nbits, 287, 25);
			if (STD_SHR(u32, diff, (uint32) tmp_1, 287, 16) == 0)
				break;
			nbits = STD_SUB(usize, nbits, 1, 288, 13);
		}
	}
	tmp_1 = STD_MUL(usize, 8, byte, 291, 5);
	return STD_ADD(usize, tmp_1, nbits, 291, 5);
}

/* encoding_summary_inet_union_family_range_t is inet_union's family_range: the smallest and largest family. */
typedef struct encoding_summary_inet_union_family_range_t
{
	uint8		f0;
	uint8		f1;
} encoding_summary_inet_union_family_range_t;

/*
 * encoding_summary_inet_union returns the network_ops key of the common
 * network of keys, Postgres' inet_gist_union with calc_inet_union_params and
 * build_inet_union_key.
 */
static bool
encoding_summary_inet_union(const encoding_summary_gist_keys_t *keys, size_t keys_len, StdVec *result,
							ChunksErrorError *error)
{
	bool		has_family_range = false;
	encoding_summary_inet_union_family_range_t family_range = {0};
	uint8		minbits = 0;
	uint8		commonbits = 0;
	StdBytes	addr = {STD_DANGLING, 0};
	uint8		family;
	uint8		max;
	size_t		size;
	volatile StdVec key;
	volatile bool key_live = false;
	StdUnwind	unwind;

	for (size_t k = 0, k_len = keys_len; k < k_len; k++)
	{
		uint8		tag = keys[k].f0;
		StdBytes	key_k = keys[k].f1;
		uint8		family_k;
		uint8		bits;
		uint8		common;
		StdBytes	other;

		if (tag != ENCODING_SUMMARY_GIST_INET)
		{
			*error = encoding_summary_corrupt();
			return false;
		}
		std_index_check(0, key_k.len, RUST_FILE, 303, 46);
		family_k = key_k.ptr[0];
		std_index_check(1, key_k.len, RUST_FILE, 303, 54);
		bits = key_k.ptr[1];
		std_index_check(2, key_k.len, RUST_FILE, 303, 62);
		common = key_k.ptr[2];
		std_range_from_check(3, key_k.len, RUST_FILE, 303, 74);
		other.ptr = key_k.ptr + 3;
		other.len = key_k.len - 3;
		if (!has_family_range)
		{
			has_family_range = true;
			family_range.f0 = family_k;
			family_range.f1 = family_k;
			minbits = bits;
			commonbits = common;
			addr = other;
		}
		else
		{
			uint8		min_2 = std_min_u8(family_range.f0, family_k);
			uint8		max_2 = std_max_u8(family_range.f1, family_k);

			family_range.f0 = min_2;
			family_range.f1 = max_2;
			minbits = std_min_u8(minbits, bits);
			commonbits = std_min_u8(commonbits, common);
			if (commonbits > 0)
			{
				size_t		tmp_1 = STD_MUL(usize, addr.len, 8, 314, 55);
				size_t		tmp_2 = std_min_usize((size_t) commonbits, tmp_1);
				size_t		tmp_3 = STD_MUL(usize, other.len, 8, 314, 75);
				size_t		n = std_min_usize(tmp_2, tmp_3);

				commonbits = (uint8) encoding_summary_bitncommon(addr.ptr, addr.len, other.ptr, other.len, n);
			}
		}
	}
	family = has_family_range ? family_range.f0 : 0;
	max = has_family_range ? family_range.f1 : 0;
	if (family != max)
	{
		family = 0;
		minbits = 0;
		commonbits = 0;
	}
	size = family == ENCODING_SUMMARY_PGSQL_AF_INET6 ? 16 : 4;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *key_v = (StdVec *) &key;
		uint8		zero = 0;
		size_t		common;
		size_t		tmp_1;
		size_t		tmp_2;
		uint8		tmp_3;
		void	   *boxed = std_box_new_uninit_layout_Checked(3, 1);

		STD_CHECK_ALIGNED(boxed, 1, 325, 19);
		STD_CHECK_NULL_DEREF(boxed, 325, 19);
		((uint8 *) boxed)[0] = family;
		((uint8 *) boxed)[1] = minbits;
		((uint8 *) boxed)[2] = commonbits;
		std_box_slice_into_vec_Checked(key_v, boxed, 3, 1);
		key_live = true;
		tmp_1 = STD_ADD(usize, 3, size, 326, 16);
		std_vec_resize(key_v, tmp_1, &zero, 1, 1);
		common = std_div_ceil_usize((size_t) commonbits, 8, RUST_FILE, 327, 40);
		tmp_1 = STD_ADD(usize, 3, common, 328, 12);
		std_range_check(3, tmp_1, key_v->len, RUST_FILE, 328, 8);
		std_range_to_check(common, addr.len, RUST_FILE, 328, 45);
		std_slice_copy_from_slice_Checked((uint8 *) key_v->ptr + 3, tmp_1 - 3, addr.ptr, common, 1, 1,
										  RUST_FILE, 328, 24);
		if (STD_REM_FOLDED(u8, commonbits, 8) != 0)
		{
			tmp_3 = (uint8) ~STD_SHR(u8, 0xFF, (uint32) STD_REM_FOLDED(u8, commonbits, 8), 330, 47);
			tmp_2 = STD_ADD(usize, 3, STD_DIV_FOLDED(usize, (size_t) commonbits, 8), 330, 13);
			std_index_check(tmp_2, key_v->len, RUST_FILE, 330, 12);
			STD_VEC_AT(key_v, uint8, tmp_2) &= tmp_3;
		}
		*result = *key_v;
		key_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (key_live)
			std_vec_free_Checked((StdVec *) &key, 1, 1);
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * EncodingSummaryRangeKeyReadClosure0 is part's captures: the bytes, and
 * where the next part starts, by reference.
 */
typedef struct EncodingSummaryRangeKeyReadClosure0
{
	const uint8 *bytes;
	size_t		bytes_len;
	size_t	   *at;
} EncodingSummaryRangeKeyReadClosure0;

/*
 * encoding_summary_RangeKey_read_closure_0 is part: a bound's part, after its
 * length as a big-endian u32.
 */
static bool
encoding_summary_RangeKey_read_closure_0(void *captures, StdBytes *result, ChunksErrorError *error)
{
	EncodingSummaryRangeKeyReadClosure0 *c = captures;
	size_t		tmp_1;
	size_t		tmp_2;
	size_t		len;

	tmp_1 = STD_ADD(usize, *c->at, 4, 354, 37);
	if (*c->at > tmp_1 || tmp_1 > c->bytes_len)
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	if (tmp_1 - *c->at != 4)
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt, RUST_FILE, 355, 57);
	len = (size_t) std_u32_from_be_bytes(c->bytes + *c->at);
	tmp_1 = STD_ADD(usize, *c->at, 4, 356, 34);
	tmp_2 = STD_ADD(usize, *c->at, 4, 356, 42);
	tmp_2 = STD_ADD(usize, tmp_2, len, 356, 42);
	if (tmp_1 > tmp_2 || tmp_2 > c->bytes_len)
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	result->ptr = c->bytes + tmp_1;
	result->len = tmp_2 - tmp_1;
	tmp_1 = STD_ADD(usize, 4, len, 357, 19);
	*c->at = STD_ADD(usize, *c->at, tmp_1, 357, 13);
	return true;
}

/*
 * EncodingSummaryRangeKeyReadClosure1 is bound's captures: the flags by
 * reference, and part by mutable reference.
 */
typedef struct EncodingSummaryRangeKeyReadClosure1
{
	const uint8 *flags;
	EncodingSummaryRangeKeyReadClosure0 *part;
} EncodingSummaryRangeKeyReadClosure1;

/*
 * encoding_summary_RangeKey_read_closure_1 is bound: a bound's two parts, or
 * None for an empty range's bound or an infinite one.
 */
static bool
encoding_summary_RangeKey_read_closure_1(void *captures, uint8 infinite, bool *has_result,
										 EncodingSummaryBound *result, ChunksErrorError *error)
{
	EncodingSummaryRangeKeyReadClosure1 *c = captures;

	switch (*c->flags & (ENCODING_SUMMARY_RANGE_EMPTY | infinite))
	{
		case 0:
			if (!encoding_summary_RangeKey_read_closure_0(c->part, &result->f0, error))
				return false;
			if (!encoding_summary_RangeKey_read_closure_0(c->part, &result->f1, error))
				return false;
			*has_result = true;
			return true;
		default:
			*has_result = false;
			return true;
	}
}

/*
 * encoding_summary_RangeKey_read returns the range key at the start of bytes
 * and how many bytes it takes.
 */
bool
encoding_summary_RangeKey_read(const uint8 *bytes, size_t bytes_len, EncodingSummaryRangeKey *result_0,
							   size_t *result_1, ChunksErrorError *error)
{
	uint8		flags;
	size_t		at = 1;
	EncodingSummaryRangeKeyReadClosure0 part = {bytes, bytes_len, &at};
	EncodingSummaryRangeKeyReadClosure1 bound = {&flags, &part};
	EncodingSummaryRangeKey key;

	if (bytes_len == 0)
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	flags = bytes[0];
	if ((flags & (ENCODING_SUMMARY_RANGE_EMPTY | ENCODING_SUMMARY_RANGE_CONTAIN_EMPTY)) ==
		(ENCODING_SUMMARY_RANGE_EMPTY | ENCODING_SUMMARY_RANGE_CONTAIN_EMPTY))
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	if (!encoding_summary_RangeKey_read_closure_1(&bound, ENCODING_SUMMARY_RANGE_LB_INF, &key.has_lower,
												  &key.lower, error))
		return false;
	if (!encoding_summary_RangeKey_read_closure_1(&bound, ENCODING_SUMMARY_RANGE_UB_INF, &key.has_upper,
												  &key.upper, error))
		return false;
	key.flags = flags;
	*result_0 = key;
	*result_1 = at;
	return true;
}

/* encoding_summary_range_union_lower_t is range_union's lower and upper: a bound's inclusive flag and the bound. */
typedef struct encoding_summary_range_union_lower_t
{
	uint8		f0;
	EncodingSummaryBound f1;
} encoding_summary_range_union_lower_t;

/*
 * encoding_summary_range_union returns the range key covering range keys,
 * Postgres' range_super_union: the empty range when every key is empty, and
 * otherwise the smallest lower bound and the largest upper bound by their
 * keys' bytes, the smaller bytes of the bound's value between equal keys,
 * with the union marked as holding an empty range when any key is or holds
 * one.
 */
static bool
encoding_summary_range_union(const encoding_summary_gist_keys_t *keys, size_t keys_len, StdVec *result,
							 ChunksErrorError *error)
{
	bool		any = false;
	bool		contain_empty = false;
	bool		has_lower = false;
	encoding_summary_range_union_lower_t lower = {0};
	bool		has_upper;
	encoding_summary_range_union_lower_t upper;
	bool		lower_inf = false;
	bool		upper_inf = false;
	uint8		flags;
	volatile StdVec out;
	volatile bool out_live = false;
	StdUnwind	unwind;

	has_upper = has_lower;
	upper = lower;
	for (size_t k = 0, k_len = keys_len; k < k_len; k++)
	{
		uint8		tag = keys[k].f0;
		StdBytes	key_k = keys[k].f1;
		EncodingSummaryRangeKey key;
		size_t		ignored;

		if (tag != ENCODING_SUMMARY_GIST_RANGE)
		{
			*error = encoding_summary_corrupt();
			return false;
		}
		if (!encoding_summary_RangeKey_read(key_k.ptr, key_k.len, &key, &ignored, error))
			return false;
		if ((key.flags & ENCODING_SUMMARY_RANGE_EMPTY) != 0)
		{
			contain_empty = true;
			continue;
		}
		any = true;
		contain_empty |= (key.flags & ENCODING_SUMMARY_RANGE_CONTAIN_EMPTY) != 0;
		lower_inf |= (key.flags & ENCODING_SUMMARY_RANGE_LB_INF) != 0;
		upper_inf |= (key.flags & ENCODING_SUMMARY_RANGE_UB_INF) != 0;
		if (key.has_lower)
		{
			EncodingSummaryBound bound = key.lower;
			bool		lowest;

			if (!has_lower)
				lowest = true;
			else
			{
				StdOrdering c = std_bytes_Ord_cmp(bound.f0.ptr, bound.f0.len, lower.f1.f0.ptr, lower.f1.f0.len);

				lowest = c != STD_ORDERING_EQUAL ? c == STD_ORDERING_LESS :
					std_bytes_Ord_cmp(bound.f1.ptr, bound.f1.len, lower.f1.f1.ptr, lower.f1.f1.len) == STD_ORDERING_LESS;
			}
			if (lowest)
			{
				if (((key.flags & ENCODING_SUMMARY_RANGE_LB_INC) != 0) !=
					(bound.f0.len > 0 && bound.f0.ptr[bound.f0.len - 1] == 0x01))
				{
					*error = encoding_summary_corrupt();
					return false;
				}
				has_lower = true;
				lower.f0 = key.flags & ENCODING_SUMMARY_RANGE_LB_INC;
				lower.f1 = bound;
			}
		}
		if (key.has_upper)
		{
			EncodingSummaryBound bound = key.upper;
			bool		highest;

			if (!has_upper)
				highest = true;
			else
				highest = std_bytes_Ord_cmp(bound.f0.ptr, bound.f0.len, upper.f1.f0.ptr, upper.f1.f0.len) ==
					STD_ORDERING_GREATER ||
					((bound.f0.len == upper.f1.f0.len &&
					  memcmp(bound.f0.ptr, upper.f1.f0.ptr, bound.f0.len) == 0) &&
					 std_bytes_Ord_cmp(bound.f1.ptr, bound.f1.len, upper.f1.f1.ptr, upper.f1.f1.len) ==
					 STD_ORDERING_LESS);
			if (highest)
			{
				if (((key.flags & ENCODING_SUMMARY_RANGE_UB_INC) != 0) !=
					(bound.f0.len > 0 && bound.f0.ptr[bound.f0.len - 1] == 0x02))
				{
					*error = encoding_summary_corrupt();
					return false;
				}
				has_upper = true;
				upper.f0 = key.flags & ENCODING_SUMMARY_RANGE_UB_INC;
				upper.f1 = bound;
			}
		}
	}
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;
		void	   *boxed;

		if (!any)
		{
			boxed = std_box_new_uninit_layout_Checked(1, 1);
			STD_CHECK_ALIGNED(boxed, 1, 411, 19);
			STD_CHECK_NULL_DEREF(boxed, 411, 19);
			*(uint8 *) boxed = ENCODING_SUMMARY_RANGE_EMPTY;
			std_box_slice_into_vec_Checked(result, boxed, 1, 1);
			std_unwind_pop(&unwind);
			return true;
		}
		has_lower = has_lower && !lower_inf;
		has_upper = has_upper && !upper_inf;
		flags = contain_empty ? ENCODING_SUMMARY_RANGE_CONTAIN_EMPTY : 0;
		flags |= (uint8) ((has_lower ? lower.f0 : ENCODING_SUMMARY_RANGE_LB_INF) |
						  (has_upper ? upper.f0 : ENCODING_SUMMARY_RANGE_UB_INF));
		boxed = std_box_new_uninit_layout_Checked(1, 1);
		STD_CHECK_ALIGNED(boxed, 1, 417, 19);
		STD_CHECK_NULL_DEREF(boxed, 417, 19);
		*(uint8 *) boxed = flags;
		std_box_slice_into_vec_Checked(out_v, boxed, 1, 1);
		out_live = true;
		for (int which = 0; which < 2; which++)
		{
			EncodingSummaryBound bound;

			if (which == 0 ? !has_lower : !has_upper)
				continue;
			bound = which == 0 ? lower.f1 : upper.f1;
			for (size_t p = 0, p_len = 2; p < p_len; p++)
			{
				StdBytes	part = p == 0 ? bound.f0 : bound.f1;
				uint8		be[4];

				std_u32_to_be_bytes((uint32) part.len, be);
				std_vec_extend_from_slice_Checked(out_v, be, 4, 1, 1);
				std_vec_extend_from_slice_Checked(out_v, part.ptr, part.len, 1, 1);
			}
		}
		*result = *out_v;
		out_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
	return true;
}

/* encoding_summary_tsvector_key_len returns the length of the tsvector_ops key at the start of bytes. */
static bool
encoding_summary_tsvector_key_len(const uint8 *bytes, size_t bytes_len, size_t *result,
								  ChunksErrorError *error)
{
	const uint8 *header;
	size_t		siglen;

	if (bytes_len < 3)
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	header = bytes;
	std_index_check(1, 3, RUST_FILE, 430, 38);
	std_index_check(2, 3, RUST_FILE, 430, 49);
	{
		uint8		be[2] = {header[1], header[2]};

		siglen = (size_t) std_u16_from_be_bytes(be);
	}
	if (siglen == 0)
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	std_index_check(0, 3, RUST_FILE, 434, 14);
	if (header[0] == ENCODING_SUMMARY_TSVECTOR_ARRKEY)
	{
		size_t		tmp_1;

		if (bytes_len < 7)
		{
			*error = encoding_summary_corrupt();
			return false;
		}
		if (7 - 3 != 4)
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
									 std_TryFromSliceError_Debug_fmt, RUST_FILE, 437, 57);
		tmp_1 = STD_MUL(usize, 4, (size_t) std_u32_from_be_bytes(bytes + 3), 437, 17);
		*result = STD_ADD(usize, 7, tmp_1, 437, 13);
	}
	else if (header[0] == ENCODING_SUMMARY_TSVECTOR_SIGNKEY)
		*result = STD_ADD(usize, 3, siglen, 439, 29);
	else if (header[0] == (ENCODING_SUMMARY_TSVECTOR_SIGNKEY | ENCODING_SUMMARY_TSVECTOR_ALLISTRUE))
		*result = 3;
	else
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	return true;
}

/* encoding_summary_tsvector_union_closure_0 is header: a key's header with flags and the first key's signature length. */
static void
encoding_summary_tsvector_union_closure_0(void *captures, uint8 flags, StdVec *result)
{
	const StdBytes *first = captures;
	uint8		header[3];

	header[0] = flags;
	std_index_check(1, first->len, RUST_FILE, 452, 34);
	header[1] = first->ptr[1];
	std_index_check(2, first->len, RUST_FILE, 452, 44);
	header[2] = first->ptr[2];
	std_string_from_Checked(result, (const char *) header, 3);
}

/*
 * encoding_summary_tsvector_union returns the signature of tsvector_ops keys
 * of one signature length, each CRC setting the bit it hashes to and each
 * signature its bits, flagged TSVECTOR_ALLISTRUE when every bit is set,
 * Postgres' gtsvector_union with the gtsvector_compress of a union.
 */
static bool
encoding_summary_tsvector_union(const encoding_summary_gist_keys_t *keys, size_t keys_len, StdVec *result,
								ChunksErrorError *error)
{
	const StdBytes *first;
	uint16		siglen;
	volatile StdVec sign;
	volatile bool sign_live = false;
	volatile StdVec out;
	volatile bool out_live = false;
	bool		all_true = false;
	StdUnwind	unwind;

	if (keys_len == 0)
	{
		*error = encoding_summary_corrupt();
		return false;
	}
	first = &keys[0].f1;
	std_index_check(1, first->len, RUST_FILE, 450, 38);
	std_index_check(2, first->len, RUST_FILE, 450, 48);
	{
		uint8		be[2] = {first->ptr[1], first->ptr[2]};

		siglen = std_u16_from_be_bytes(be);
	}
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *sign_v = (StdVec *) &sign;
		StdVec	   *out_v = (StdVec *) &out;
		bool		all_ff = true;

		std_vec_from_elem_zeroed(sign_v, (size_t) siglen, 1, 1);
		sign_live = true;
		for (size_t k = 0, k_len = keys_len; k < k_len; k++)
		{
			uint8		tag = keys[k].f0;
			StdBytes	key = keys[k].f1;
			bool		bad = tag != ENCODING_SUMMARY_GIST_TSVECTOR;

			if (!bad)
			{
				std_range_check(1, 3, key.len, RUST_FILE, 455, 39);
				std_range_check(1, 3, first->len, RUST_FILE, 455, 54);
				bad = memcmp(key.ptr + 1, first->ptr + 1, 2) != 0;
			}
			if (bad)
			{
				*error = encoding_summary_corrupt();
				sign_live = false;
				std_vec_free_Checked(sign_v, 1, 1);
				std_unwind_pop(&unwind);
				return false;
			}
			std_index_check(0, key.len, RUST_FILE, 458, 15);
			switch (key.ptr[0])
			{
				case ENCODING_SUMMARY_TSVECTOR_ARRKEY:
					{
						const uint8 *chunks;
						size_t		chunks_len;
						const uint8 *remainder;
						size_t		remainder_len;

						std_range_from_check(7, key.len, RUST_FILE, 460, 31);
						std_slice_as_chunks(key.ptr + 7, key.len - 7, 1, 1, 4, (const void **) &chunks, &chunks_len,
											(const void **) &remainder, &remainder_len, RUST_FILE, 460, 37);
						for (size_t c = 0, c_len = chunks_len; c < c_len; c++)
						{
							size_t		tmp_1 = STD_MUL(usize, sign_v->len, 8, 461, 68);
							size_t		bit = STD_REM(usize, (size_t) std_u32_from_be_bytes(chunks + 4 * c), tmp_1,
													  461, 31);
							size_t		tmp_2;
							uint8		tmp_3;

							tmp_3 = STD_SHL(u8, 1, (uint32) STD_REM_FOLDED(usize, bit, 8), 462, 38);
							tmp_2 = STD_DIV_FOLDED(usize, bit, 8);
							std_index_check(tmp_2, sign_v->len, RUST_FILE, 462, 25);
							STD_VEC_AT(sign_v, uint8, tmp_2) |= tmp_3;
						}
					}
					break;
				case ENCODING_SUMMARY_TSVECTOR_SIGNKEY:
					std_range_from_check(3, key.len, RUST_FILE, 466, 55);
					for (size_t s = 0, s_len = std_min_usize(sign_v->len, key.len - 3); s < s_len; s++)
						STD_VEC_AT(sign_v, uint8, s) |= key.ptr[3 + s];
					break;
				default:
					all_true = true;
					break;
			}
		}
		if (!all_true)
		{
			for (size_t s = 0, s_len = sign_v->len; s < s_len; s++)
			{
				if (STD_VEC_AT(sign_v, uint8, s) != 0xff)
				{
					all_ff = false;
					break;
				}
			}
		}
		if (all_true || all_ff)
		{
			encoding_summary_tsvector_union_closure_0((void *) first,
													  ENCODING_SUMMARY_TSVECTOR_SIGNKEY |
													  ENCODING_SUMMARY_TSVECTOR_ALLISTRUE,
													  result);
			sign_live = false;
			std_vec_free_Checked(sign_v, 1, 1);
			std_unwind_pop(&unwind);
			return true;
		}
		encoding_summary_tsvector_union_closure_0((void *) first, ENCODING_SUMMARY_TSVECTOR_SIGNKEY, out_v);
		out_live = true;
		std_vec_extend_from_slice_Checked(out_v, sign_v->ptr, sign_v->len, 1, 1);
		*result = *out_v;
		out_live = false;
		sign_live = false;
		std_vec_free_Checked(sign_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		if (sign_live)
			std_vec_free_Checked((StdVec *) &sign, 1, 1);
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * encoding_summary_tsquery_union returns the signature of tsquery_ops keys,
 * every bit any of them sets, Postgres' gtsquery_union.
 */
static bool
encoding_summary_tsquery_union(const encoding_summary_gist_keys_t *keys, size_t keys_len, StdVec *result,
							   ChunksErrorError *error)
{
	uint64		sign = 0;
	uint8		be[8];

	for (size_t k = 0, k_len = keys_len; k < k_len; k++)
	{
		uint8		tag = keys[k].f0;
		StdBytes	key = keys[k].f1;

		if (tag != ENCODING_SUMMARY_GIST_TSQUERY)
		{
			*error = encoding_summary_corrupt();
			return false;
		}
		if (key.len != 8)
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1, STD_DANGLING,
									 std_TryFromSliceError_Debug_fmt, RUST_FILE, 488, 51);
		sign |= std_u64_from_be_bytes(key.ptr);
	}
	std_u64_to_be_bytes(sign, be);
	std_string_from_Checked(result, (const char *) be, 8);
	return true;
}

#ifdef DOLTGRES_TEST_ENCODING

/* The key::range instance at an i32 bound, which the tests' range writes. */
#define ENCODING_KEY_SCOPE static
#define ENCODING_KEY_DECLARE
#define ENCODING_KEY_DEFINE
#define ENCODING_KEY_RANGE_TS I32
#define ENCODING_KEY_RANGE_T int32
#define ENCODING_KEY_RANGE_T_COPY
#include "doltgres/encoding/key_template.h"

/* encoding_summary_tests_value_keys_t is value's key: a tag and its bytes. */
typedef struct encoding_summary_tests_value_keys_t
{
	uint8		f0;
	StdVec		f1;
} encoding_summary_tests_value_keys_t;

/* encoding_summary_tests_VecU8_Debug_fmt is a Vec<u8>'s Debug: its bytes as a list. */
static bool
encoding_summary_tests_VecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *v = (const StdVec *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&b, &STD_VEC_AT(v, uint8, i), std_u8_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* encoding_summary_tests_Array39U8_Debug_fmt is a [u8; 39]'s Debug: its bytes as a list. */
static bool
encoding_summary_tests_Array39U8_Debug_fmt(const void *value, StdFormatter *f)
{
	const uint8 *a = (const uint8 *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (int i = 0; i < 39; i++)
		std_DebugList_entry(&b, &a[i], std_u8_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* encoding_summary_tests_VecU8_PartialEq_eq is Vec<u8>'s PartialEq: the same bytes. */
static bool
encoding_summary_tests_VecU8_PartialEq_eq(const StdVec *a, const StdVec *b)
{
	return a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
}

/* encoding_summary_tests_doubles returns doubles' bytes. */
static void
encoding_summary_tests_doubles(const float8 *values, size_t values_len, StdVec *result)
{
	volatile StdVec out;
	volatile bool out_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;
		StdSizeHint iter = {values_len, true, values_len};
		StdSizeHint hint = std_FlattenCompat_size_hint(iter, 8);

		std_vec_with_capacity_trusted(out_v, hint, 1, 1);
		out_live = true;
		std_vec_extend_trusted_reserve(out_v, hint, 1, 1);
		for (size_t i = 0, i_len = values_len; i < i_len; i++)
		{
			uint8		bytes[8];

			std_f64_to_le_bytes(values[i], bytes);
			for (size_t j = 0, j_len = 8; j < j_len; j++)
			{
				STD_VEC_AT(out_v, uint8, out_v->len) = bytes[j];
				out_v->len++;
			}
		}
		*result = *out_v;
		out_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* encoding_summary_tests_value returns a leaf value of tagged keys. */
static void
encoding_summary_tests_value(const encoding_summary_tests_value_keys_t *keys, size_t keys_len, StdVec *result)
{
	volatile StdVec out;
	volatile bool out_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;
		uint8	   *boxed = std_box_new_uninit_layout_Checked(1, 1);

		STD_CHECK_ALIGNED(boxed, 1, 504, 23);
		STD_CHECK_NULL_DEREF(boxed, 504, 23);
		boxed[0] = (uint8) keys_len;
		std_box_slice_into_vec_Checked(out_v, boxed, 1, 1);
		out_live = true;
		for (size_t i = 0, i_len = keys_len; i < i_len; i++)
		{
			std_vec_u8_push_Checked(out_v, keys[i].f0);
			std_vec_extend_from_slice_Checked(out_v, keys[i].f1.ptr, keys[i].f1.len, 1, 1);
		}
		*result = *out_v;
		out_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* encoding_summary_tests_Array34U8_Debug_fmt is a [u8; 34]'s Debug: its bytes as a list. */
static bool
encoding_summary_tests_Array34U8_Debug_fmt(const void *value, StdFormatter *f)
{
	const uint8 *a = (const uint8 *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (int i = 0; i < 34; i++)
		std_DebugList_entry(&b, &a[i], std_u8_Debug_fmt);
	return std_DebugList_finish(&b);
}

/*
 * test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0
 * is union: the GiST union of leaf values, unwrapped.
 */
static void
test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(void *captures,
																					   const StdVec *items, size_t items_len,
																					   StdVec *result)
{
	volatile StdVec slices;
	volatile bool slices_live = false;
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *slices_v = (StdVec *) &slices;
		StdSizeHint hint = {items_len, true, items_len};

		std_vec_with_capacity_trusted(slices_v, hint, sizeof(StdBytes), _Alignof(StdBytes));
		slices_live = true;
		std_vec_extend_trusted_reserve(slices_v, hint, sizeof(StdBytes), _Alignof(StdBytes));
		for (size_t i = 0, i_len = items_len; i < i_len; i++)
		{
			StdBytes	slice = {items[i].ptr, items[i].len};

			STD_VEC_AT(slices_v, StdBytes, slices_v->len) = slice;
			slices_v->len++;
		}
		if (!encoding_summary_gist_union(0, slices_v->ptr, slices_v->len, result, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 514, 109);
		}
		slices_live = false;
		std_vec_free_Checked(slices_v, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		if (slices_live)
			std_vec_free_Checked((StdVec *) &slices, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_1
 * is v4: the leaf value of an IPv4 inet key of an address and its bits.
 */
static void
test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_1(void *captures,
																					   const uint8 *a, uint8 bits,
																					   StdVec *result)
{
	volatile encoding_summary_tests_value_keys_t keys[1];
	volatile bool keys_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		encoding_summary_tests_value_keys_t *keys_v = (encoding_summary_tests_value_keys_t *) keys;
		uint8		head[3] = {2, bits, 32};
		StdBytes	parts[2];

		parts[0].ptr = head;
		parts[0].len = sizeof(head);
		parts[1].ptr = a;
		parts[1].len = 4;
		keys_v[0].f0 = ENCODING_SUMMARY_GIST_INET;
		std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, &keys_v[0].f1);
		keys_live = true;
		encoding_summary_tests_value(keys_v, 1, result);
		keys_live = false;
		std_vec_free_Checked(&keys_v[0].f1, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (keys_live)
			std_vec_free_Checked((StdVec *) &keys[0].f1, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* The each-columns test's locals, by their order of declaration, which the landing pad drops in reverse. */
enum
{
	EACH_TEST_A,
	EACH_TEST_B,
	EACH_TEST_BOXES,
	EACH_TEST_NAN,
	EACH_TEST_WIDENED,
	EACH_TEST_P,
	EACH_TEST_Q,
	EACH_TEST_POINTS,
	EACH_TEST_NETWORKS,
	EACH_TEST_V6,
	EACH_TEST_WITH_INCLUDE,
	EACH_TEST_COUNTED,
	EACH_TEST_VECS,
};

/*
 * test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored:
 * box, point, inet, and null unions keep their keys' bytes as they are
 * stored, a NaN coordinate widens a box to the infinities, an index's
 * included columns after the keys are left out, and counted summaries skip
 * their count.
 */
void
test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored(void)
{
	volatile StdVec vecs[EACH_TEST_VECS];
	volatile bool live[EACH_TEST_VECS] = {0};
	volatile encoding_summary_tests_value_keys_t key[2];
	volatile bool key_live[2] = {0};
	volatile StdVec items[2];
	volatile bool items_live[2] = {0};
	volatile StdVec value_temp;
	volatile bool value_temp_live = false;
	volatile StdVec unwrapped;
	volatile bool unwrapped_live = false;
	volatile StdVec doubles_temp;
	volatile bool doubles_temp_live = false;
	volatile StdVec expected;
	volatile bool expected_live = false;
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const float8 doubles_a[] = {2.0, 3.0, 0.0, 1.0};
		static const float8 doubles_b[] = {5.0, 1.5, -1.0, 0.5};
		static const uint8 boxes_expected[34] = {
			1, 1, 0, 0, 0, 0, 0, 0, 20, 64, 0, 0, 0, 0, 0, 0, 8, 64, 0, 0, 0, 0, 0, 0, 240, 191, 0, 0, 0, 0, 0, 0,
			224, 63,
		};
		static const float8 doubles_p[] = {1.0, -2.0};
		static const float8 doubles_q[] = {-3.0, 4.0};
		static const float8 doubles_points[] = {1.0, 4.0, -3.0, -2.0};
		static const uint8 points_head[] = {2, ENCODING_SUMMARY_GIST_BOX};
		static const uint8 points_tail[] = {ENCODING_SUMMARY_GIST_NULL};
		static const uint8 network_1[4] = {192, 168, 1, 7};
		static const uint8 network_2[4] = {192, 168, 3, 9};
		static const uint8 v6_head[] = {3, 128, 128};
		static const uint8 v6_prefix[] = {0x20, 1, 0x0d, 0xb8};
		static const uint8 v6_zeros[12] = {0};
		static const uint8 counted_head[] = {3};
		static const float8 doubles_counted[] = {1.0, -2.0};
		static const uint8 summary_head[] = {1, ENCODING_SUMMARY_GIST_BOX};
		static const float8 doubles_summary[] = {1.0, -2.0, 1.0, -2.0};
		static const uint8 bad_box[] = {1, ENCODING_SUMMARY_GIST_BOX, 0};
		static const uint8 bad_kind[] = {1, 9};
		StdVec	   *v = (StdVec *) vecs;
		StdVec	   *item = (StdVec *) items;
		encoding_summary_tests_value_keys_t *key_v = (encoding_summary_tests_value_keys_t *) key;
		StdVec	   *value_temp_v = (StdVec *) &value_temp;
		StdVec	   *unwrapped_v = (StdVec *) &unwrapped;
		StdVec	   *doubles_temp_v = (StdVec *) &doubles_temp;
		StdVec	   *expected_v = (StdVec *) &expected;
		float8		nan_values[4];
		float8		left[2];
		float8		right[2];
		StdBytes	parts[3];
		StdBytes	slices[1];
		EncodingSummarySummarizer summarize;
		EncodingSummarySummarizer tmp_1;
		uint8	   *boxed;
		bool		equal;
		bool		ok;
		bool		is_err;

		key_v[0].f0 = ENCODING_SUMMARY_GIST_BOX;
		encoding_summary_tests_doubles(doubles_a, lengthof(doubles_a), &key_v[0].f1);
		key_live[0] = true;
		encoding_summary_tests_value(key_v, 1, &v[EACH_TEST_A]);
		live[EACH_TEST_A] = true;
		key_live[0] = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		key_v[0].f0 = ENCODING_SUMMARY_GIST_BOX;
		encoding_summary_tests_doubles(doubles_b, lengthof(doubles_b), &key_v[0].f1);
		key_live[0] = true;
		encoding_summary_tests_value(key_v, 1, &v[EACH_TEST_B]);
		live[EACH_TEST_B] = true;
		key_live[0] = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		std_vec_clone(&item[0], &v[EACH_TEST_A], 1, 1);
		items_live[0] = true;
		item[1] = v[EACH_TEST_B];
		live[EACH_TEST_B] = false;
		items_live[1] = true;
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																							   &v[EACH_TEST_BOXES]);
		live[EACH_TEST_BOXES] = true;
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		equal = v[EACH_TEST_BOXES].len == sizeof(boxes_expected) &&
			memcmp(v[EACH_TEST_BOXES].ptr, boxes_expected, sizeof(boxes_expected)) == 0;
		STD_ASSERT_EQ(518, 9, equal, &v[EACH_TEST_BOXES], encoding_summary_tests_VecU8_Debug_fmt, boxes_expected,
					  encoding_summary_tests_Array34U8_Debug_fmt);
		std_vec_clone(&item[0], &v[EACH_TEST_BOXES], 1, 1);
		items_live[0] = true;
		std_vec_clone(&item[1], &v[EACH_TEST_A], 1, 1);
		items_live[1] = true;
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																							   unwrapped_v);
		unwrapped_live = true;
		STD_ASSERT_EQ(525, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, &v[EACH_TEST_BOXES]),
					  unwrapped_v, encoding_summary_tests_VecU8_Debug_fmt, &v[EACH_TEST_BOXES],
					  encoding_summary_tests_VecU8_Debug_fmt);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		nan_values[0] = NAN;
		nan_values[1] = 0.0;
		nan_values[2] = 0.0;
		nan_values[3] = 0.0;
		key_v[0].f0 = ENCODING_SUMMARY_GIST_BOX;
		encoding_summary_tests_doubles(nan_values, lengthof(nan_values), &key_v[0].f1);
		key_live[0] = true;
		encoding_summary_tests_value(key_v, 1, &v[EACH_TEST_NAN]);
		live[EACH_TEST_NAN] = true;
		key_live[0] = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		item[0] = v[EACH_TEST_A];
		live[EACH_TEST_A] = false;
		items_live[0] = true;
		item[1] = v[EACH_TEST_NAN];
		live[EACH_TEST_NAN] = false;
		items_live[1] = true;
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																							   &v[EACH_TEST_WIDENED]);
		live[EACH_TEST_WIDENED] = true;
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		left[0] = encoding_summary_double(v[EACH_TEST_WIDENED].ptr, v[EACH_TEST_WIDENED].len, 2);
		left[1] = encoding_summary_double(v[EACH_TEST_WIDENED].ptr, v[EACH_TEST_WIDENED].len, 18);
		right[0] = INFINITY;
		right[1] = -INFINITY;
		STD_ASSERT_EQ(528, 9, left[0] == right[0] && left[1] == right[1],
					  &STD_TUPLE(2, STD_ITEM(&left[0], std_f64_Debug_fmt), STD_ITEM(&left[1], std_f64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right[0], std_f64_Debug_fmt), STD_ITEM(&right[1], std_f64_Debug_fmt)),
					  std_tuple_Debug_fmt);
		left[0] = encoding_summary_double(v[EACH_TEST_WIDENED].ptr, v[EACH_TEST_WIDENED].len, 10);
		left[1] = encoding_summary_double(v[EACH_TEST_WIDENED].ptr, v[EACH_TEST_WIDENED].len, 26);
		right[0] = 3.0;
		right[1] = 0.0;
		STD_ASSERT_EQ(529, 9, left[0] == right[0] && left[1] == right[1],
					  &STD_TUPLE(2, STD_ITEM(&left[0], std_f64_Debug_fmt), STD_ITEM(&left[1], std_f64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right[0], std_f64_Debug_fmt), STD_ITEM(&right[1], std_f64_Debug_fmt)),
					  std_tuple_Debug_fmt);
		key_v[0].f0 = ENCODING_SUMMARY_GIST_POINT;
		encoding_summary_tests_doubles(doubles_p, lengthof(doubles_p), &key_v[0].f1);
		key_live[0] = true;
		key_v[1].f0 = ENCODING_SUMMARY_GIST_NULL;
		std_vec_new(&key_v[1].f1, 1, 1);
		key_live[1] = true;
		encoding_summary_tests_value(key_v, 2, &v[EACH_TEST_P]);
		live[EACH_TEST_P] = true;
		key_live[0] = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		key_live[1] = false;
		std_vec_free_Checked(&key_v[1].f1, 1, 1);
		key_v[0].f0 = ENCODING_SUMMARY_GIST_POINT;
		encoding_summary_tests_doubles(doubles_q, lengthof(doubles_q), &key_v[0].f1);
		key_live[0] = true;
		key_v[1].f0 = ENCODING_SUMMARY_GIST_NULL;
		std_vec_new(&key_v[1].f1, 1, 1);
		key_live[1] = true;
		encoding_summary_tests_value(key_v, 2, &v[EACH_TEST_Q]);
		live[EACH_TEST_Q] = true;
		key_live[0] = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		key_live[1] = false;
		std_vec_free_Checked(&key_v[1].f1, 1, 1);
		item[0] = v[EACH_TEST_P];
		live[EACH_TEST_P] = false;
		items_live[0] = true;
		item[1] = v[EACH_TEST_Q];
		live[EACH_TEST_Q] = false;
		items_live[1] = true;
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																							   &v[EACH_TEST_POINTS]);
		live[EACH_TEST_POINTS] = true;
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		encoding_summary_tests_doubles(doubles_points, lengthof(doubles_points), doubles_temp_v);
		doubles_temp_live = true;
		parts[0].ptr = points_head;
		parts[0].len = sizeof(points_head);
		parts[1].ptr = doubles_temp_v->ptr;
		parts[1].len = doubles_temp_v->len;
		parts[2].ptr = points_tail;
		parts[2].len = sizeof(points_tail);
		std_slice_concat(parts, 3, sizeof(StdBytes), 1, 1, expected_v);
		expected_live = true;
		STD_ASSERT_EQ(534, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[EACH_TEST_POINTS], expected_v),
					  &v[EACH_TEST_POINTS], encoding_summary_tests_VecU8_Debug_fmt, expected_v,
					  encoding_summary_tests_VecU8_Debug_fmt);
		expected_live = false;
		std_vec_free_Checked(expected_v, 1, 1);
		doubles_temp_live = false;
		std_vec_free_Checked(doubles_temp_v, 1, 1);
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(NULL, &v[EACH_TEST_POINTS],
																							   1, unwrapped_v);
		unwrapped_live = true;
		STD_ASSERT_EQ(535, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, &v[EACH_TEST_POINTS]),
					  unwrapped_v, encoding_summary_tests_VecU8_Debug_fmt, &v[EACH_TEST_POINTS],
					  encoding_summary_tests_VecU8_Debug_fmt);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_1(NULL, network_1, 32,
																							   &item[0]);
		items_live[0] = true;
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_1(NULL, network_2, 24,
																							   &item[1]);
		items_live[1] = true;
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																							   &v[EACH_TEST_NETWORKS]);
		live[EACH_TEST_NETWORKS] = true;
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		boxed = std_box_new_uninit_layout_Checked(9, 1);
		STD_CHECK_ALIGNED(boxed, 1, 539, 30);
		STD_CHECK_NULL_DEREF(boxed, 539, 30);
		boxed[0] = 1;
		boxed[1] = ENCODING_SUMMARY_GIST_INET;
		boxed[2] = 2;
		boxed[3] = 24;
		boxed[4] = 22;
		boxed[5] = 192;
		boxed[6] = 168;
		boxed[7] = 0;
		boxed[8] = 0;
		std_box_slice_into_vec_Checked(expected_v, boxed, 9, 1);
		expected_live = true;
		STD_ASSERT_EQ(539, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[EACH_TEST_NETWORKS], expected_v),
					  &v[EACH_TEST_NETWORKS], encoding_summary_tests_VecU8_Debug_fmt, expected_v,
					  encoding_summary_tests_VecU8_Debug_fmt);
		expected_live = false;
		std_vec_free_Checked(expected_v, 1, 1);
		parts[0].ptr = v6_head;
		parts[0].len = sizeof(v6_head);
		parts[1].ptr = v6_prefix;
		parts[1].len = sizeof(v6_prefix);
		parts[2].ptr = v6_zeros;
		parts[2].len = sizeof(v6_zeros);
		key_v[0].f0 = ENCODING_SUMMARY_GIST_INET;
		std_slice_concat(parts, 3, sizeof(StdBytes), 1, 1, &key_v[0].f1);
		key_live[0] = true;
		encoding_summary_tests_value(key_v, 1, &v[EACH_TEST_V6]);
		live[EACH_TEST_V6] = true;
		key_live[0] = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		item[0] = v[EACH_TEST_NETWORKS];
		live[EACH_TEST_NETWORKS] = false;
		items_live[0] = true;
		item[1] = v[EACH_TEST_V6];
		live[EACH_TEST_V6] = false;
		items_live[1] = true;
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																							   unwrapped_v);
		unwrapped_live = true;
		boxed = std_box_new_uninit_layout_Checked(9, 1);
		STD_CHECK_ALIGNED(boxed, 1, 541, 44);
		STD_CHECK_NULL_DEREF(boxed, 541, 44);
		boxed[0] = 1;
		boxed[1] = ENCODING_SUMMARY_GIST_INET;
		boxed[2] = 0;
		boxed[3] = 0;
		boxed[4] = 0;
		boxed[5] = 0;
		boxed[6] = 0;
		boxed[7] = 0;
		boxed[8] = 0;
		std_box_slice_into_vec_Checked(expected_v, boxed, 9, 1);
		expected_live = true;
		STD_ASSERT_EQ(541, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, expected_v), unwrapped_v,
					  encoding_summary_tests_VecU8_Debug_fmt, expected_v, encoding_summary_tests_VecU8_Debug_fmt);
		expected_live = false;
		std_vec_free_Checked(expected_v, 1, 1);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		key_v[0].f0 = ENCODING_SUMMARY_GIST_NULL;
		std_vec_new(&key_v[0].f1, 1, 1);
		key_live[0] = true;
		encoding_summary_tests_value(key_v, 1, &item[0]);
		items_live[0] = true;
		std_string_from_Checked(&item[1], "include", 7);
		items_live[1] = true;
		parts[0].ptr = item[0].ptr;
		parts[0].len = item[0].len;
		parts[1].ptr = item[1].ptr;
		parts[1].len = item[1].len;
		std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, &v[EACH_TEST_WITH_INCLUDE]);
		live[EACH_TEST_WITH_INCLUDE] = true;
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		key_live[0] = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		item[0] = v[EACH_TEST_WITH_INCLUDE];
		live[EACH_TEST_WITH_INCLUDE] = false;
		items_live[0] = true;
		test_encoding_summary_tests_each_columns_union_bytes_stay_as_they_are_stored_closure_0(NULL, item, 1,
																							   unwrapped_v);
		unwrapped_live = true;
		boxed = std_box_new_uninit_layout_Checked(2, 1);
		STD_CHECK_ALIGNED(boxed, 1, 544, 44);
		STD_CHECK_NULL_DEREF(boxed, 544, 44);
		boxed[0] = 1;
		boxed[1] = ENCODING_SUMMARY_GIST_NULL;
		std_box_slice_into_vec_Checked(expected_v, boxed, 2, 1);
		expected_live = true;
		STD_ASSERT_EQ(544, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, expected_v), unwrapped_v,
					  encoding_summary_tests_VecU8_Debug_fmt, expected_v, encoding_summary_tests_VecU8_Debug_fmt);
		expected_live = false;
		std_vec_free_Checked(expected_v, 1, 1);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		key_v[0].f0 = ENCODING_SUMMARY_GIST_POINT;
		encoding_summary_tests_doubles(doubles_counted, lengthof(doubles_counted), &key_v[0].f1);
		key_live[0] = true;
		encoding_summary_tests_value(key_v, 1, value_temp_v);
		value_temp_live = true;
		parts[0].ptr = counted_head;
		parts[0].len = sizeof(counted_head);
		parts[1].ptr = value_temp_v->ptr;
		parts[1].len = value_temp_v->len;
		std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, &v[EACH_TEST_COUNTED]);
		live[EACH_TEST_COUNTED] = true;
		value_temp_live = false;
		std_vec_free_Checked(value_temp_v, 1, 1);
		key_live[0] = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		if (!encoding_summary_summarizer(ENCODING_SUMMARY_GIST_COUNTED, &tmp_1, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 546, 50);
		}
		if (tmp_1 == NULL)
			std_option_unwrap_failed(RUST_FILE, 546, 59);
		summarize = tmp_1;
		slices[0].ptr = v[EACH_TEST_COUNTED].ptr;
		slices[0].len = v[EACH_TEST_COUNTED].len;
		if (!summarize(0, slices, 1, unwrapped_v, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 548, 39);
		}
		unwrapped_live = true;
		encoding_summary_tests_doubles(doubles_summary, lengthof(doubles_summary), doubles_temp_v);
		doubles_temp_live = true;
		parts[0].ptr = summary_head;
		parts[0].len = sizeof(summary_head);
		parts[1].ptr = doubles_temp_v->ptr;
		parts[1].len = doubles_temp_v->len;
		std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, expected_v);
		expected_live = true;
		STD_ASSERT_EQ(547, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, expected_v), unwrapped_v,
					  encoding_summary_tests_VecU8_Debug_fmt, expected_v, encoding_summary_tests_VecU8_Debug_fmt);
		expected_live = false;
		std_vec_free_Checked(expected_v, 1, 1);
		doubles_temp_live = false;
		std_vec_free_Checked(doubles_temp_v, 1, 1);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		if (!encoding_summary_summarizer(0, &tmp_1, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 551, 31);
		}
		STD_ASSERT(551, 9, tmp_1 == NULL, "summarizer(0).unwrap().is_none()");
		ok = encoding_summary_summarizer(5, &tmp_1, (ChunksErrorError *) &error);
		if (!ok)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		STD_ASSERT(552, 9, !ok, "summarizer(5).is_err()");
		slices[0].ptr = bad_box;
		slices[0].len = sizeof(bad_box);
		is_err = !encoding_summary_gist_union(0, slices, 1, unwrapped_v, (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		STD_ASSERT(553, 9, is_err, "gist_union(0, &[&[1, GIST_BOX, 0]]).is_err()");
		slices[0].ptr = bad_kind;
		slices[0].len = sizeof(bad_kind);
		is_err = !encoding_summary_gist_union(0, slices, 1, unwrapped_v, (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		STD_ASSERT(554, 9, is_err, "gist_union(0, &[&[1, 9]]).is_err()");
		for (int k = EACH_TEST_VECS - 1; k >= 0; k--)
		{
			if (live[k])
			{
				live[k] = false;
				std_vec_free_Checked(&v[k], 1, 1);
			}
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		if (expected_live)
			std_vec_free_Checked((StdVec *) &expected, 1, 1);
		if (doubles_temp_live)
			std_vec_free_Checked((StdVec *) &doubles_temp, 1, 1);
		if (unwrapped_live)
			std_vec_free_Checked((StdVec *) &unwrapped, 1, 1);
		for (int k = 0; k < 2; k++)
		{
			if (items_live[k])
				std_vec_free_Checked((StdVec *) &items[k], 1, 1);
		}
		if (value_temp_live)
			std_vec_free_Checked((StdVec *) &value_temp, 1, 1);
		for (int k = 0; k < 2; k++)
		{
			if (key_live[k])
				std_vec_free_Checked((StdVec *) &key[k].f1, 1, 1);
		}
		for (int k = EACH_TEST_VECS - 1; k >= 0; k--)
		{
			if (live[k])
				std_vec_free_Checked((StdVec *) &vecs[k], 1, 1);
		}
		std_unwind_resume(&unwind);
	}
}

/* encoding_summary_tests_key_i32_FnMut_call_mut is key::i32 called as the FnMut range takes, its shim. */
static void
encoding_summary_tests_key_i32_FnMut_call_mut(void *captures, StdVec *out, int32 *value)
{
	encoding_key_i32(out, *value);
}

/*
 * encoding_summary_tests_range_lower_t is range's bound: a value, whether it
 * is inclusive, and its stored bytes.
 */
typedef struct encoding_summary_tests_range_lower_t
{
	int32		f0;
	bool		f1;
	StdBytes	f2;
} encoding_summary_tests_range_lower_t;

/*
 * encoding_summary_tests_range returns the range key of an int4 range's flags
 * and stored bounds, each a value and whether it is inclusive, with a value's
 * stored bytes after it.
 */
static void
encoding_summary_tests_range(uint8 flags, bool has_lower, encoding_summary_tests_range_lower_t lower,
							 bool has_upper, encoding_summary_tests_range_lower_t upper, StdVec *result)
{
	volatile StdVec out;
	volatile bool out_live = false;
	volatile StdVec key;
	volatile bool key_live = false;
	volatile StdVec part;
	volatile bool part_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *out_v = (StdVec *) &out;
		StdVec	   *key_v = (StdVec *) &key;
		StdVec	   *part_v = (StdVec *) &part;
		uint8	   *boxed = std_box_new_uninit_layout_Checked(1, 1);
		bool		has_bounds[2] = {has_lower, has_upper};
		encoding_summary_tests_range_lower_t bounds[2];
		bool		is_lower[2] = {true, false};
		encoding_summary_tests_value_keys_t keys[1];

		STD_CHECK_ALIGNED(boxed, 1, 560, 23);
		STD_CHECK_NULL_DEREF(boxed, 560, 23);
		boxed[0] = flags;
		std_box_slice_into_vec_Checked(out_v, boxed, 1, 1);
		out_live = true;
		bounds[0] = lower;
		bounds[1] = upper;
		for (size_t start = 0, n; std_array_IntoIter_next(&start, 2, 2, &n);)
		{
			encoding_summary_tests_range_lower_t bound = bounds[n];
			StdBytes	parts[2];

			if (!has_bounds[n])
				continue;
			std_vec_new(key_v, 1, 1);
			key_live = true;
			if (is_lower[n])
			{
				encoding_key_rangeI32_lower_t value = {bound.f0, bound.f1};
				encoding_key_rangeI32_lower_t none = {0};
				size_t		end;

				encoding_key_rangeI32(key_v, true, value, false, none, encoding_summary_tests_key_i32_FnMut_call_mut,
									  NULL, NULL);
				end = STD_SUB(usize, key_v->len, 1, 567, 28);
				std_range_check(1, end, key_v->len, RUST_FILE, 567, 24);
				std_string_from_Checked(part_v, (const char *) key_v->ptr + 1, end - 1);
			}
			else
			{
				encoding_key_rangeI32_lower_t value = {bound.f0, bound.f1};
				encoding_key_rangeI32_lower_t none = {0};

				encoding_key_rangeI32(key_v, false, none, true, value, encoding_summary_tests_key_i32_FnMut_call_mut,
									  NULL, NULL);
				std_range_from_check(2, key_v->len, RUST_FILE, 571, 24);
				std_string_from_Checked(part_v, (const char *) key_v->ptr + 2, key_v->len - 2);
			}
			part_live = true;
			parts[0].ptr = part_v->ptr;
			parts[0].len = part_v->len;
			parts[1] = bound.f2;
			for (size_t pstart = 0, p; std_array_IntoIter_next(&pstart, 2, 2, &p);)
			{
				uint8		be[4];

				std_u32_to_be_bytes((uint32) parts[p].len, be);
				std_vec_extend_from_slice_Checked(out_v, be, 4, 1, 1);
				std_vec_extend_from_slice_Checked(out_v, parts[p].ptr, parts[p].len, 1, 1);
			}
			part_live = false;
			std_vec_free_Checked(part_v, 1, 1);
			key_live = false;
			std_vec_free_Checked(key_v, 1, 1);
		}
		keys[0].f0 = ENCODING_SUMMARY_GIST_RANGE;
		keys[0].f1 = *out_v;
		encoding_summary_tests_value(keys, 1, result);
		out_live = false;
		std_vec_free_Checked(&keys[0].f1, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (part_live)
			std_vec_free_Checked((StdVec *) &part, 1, 1);
		if (key_live)
			std_vec_free_Checked((StdVec *) &key, 1, 1);
		if (out_live)
			std_vec_free_Checked((StdVec *) &out, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0
 * is union: the GiST union of leaf values, unwrapped.
 */
static void
test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(
																												void *captures, const StdVec *const *items, size_t items_len, StdVec *result)
{
	volatile StdVec slices;
	volatile bool slices_live = false;
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *slices_v = (StdVec *) &slices;
		StdSizeHint hint = {items_len, true, items_len};

		std_vec_with_capacity_trusted(slices_v, hint, sizeof(StdBytes), _Alignof(StdBytes));
		slices_live = true;
		std_vec_extend_trusted_reserve(slices_v, hint, sizeof(StdBytes), _Alignof(StdBytes));
		for (size_t i = 0, i_len = items_len; i < i_len; i++)
		{
			StdBytes	slice = {items[i]->ptr, items[i]->len};

			STD_VEC_AT(slices_v, StdBytes, slices_v->len) = slice;
			slices_v->len++;
		}
		if (!encoding_summary_gist_union(0, slices_v->ptr, slices_v->len, result, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 585, 105);
		}
		slices_live = false;
		std_vec_free_Checked(slices_v, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		if (slices_live)
			std_vec_free_Checked((StdVec *) &slices, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1
 * is le: an i32's little-endian bytes.
 */
static void
test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(
																												void *captures, int32 v, uint8 *result)
{
	std_u32_to_le_bytes((uint32) v, result);
}

/* The test's vectors, by their order of declaration, which the landing pad drops in reverse. */
enum
{
	RANGE_TEST_A,
	RANGE_TEST_B,
	RANGE_TEST_EMPTY,
	RANGE_TEST_BELOW,
	RANGE_TEST_AB,
	RANGE_TEST_WITH_EMPTY,
	RANGE_TEST_EXCLUSIVE,
	RANGE_TEST_X,
	RANGE_TEST_Y,
	RANGE_TEST_WITH_REST,
	RANGE_TEST_KEYS,
	RANGE_TEST_T0,
	RANGE_TEST_T1,
	RANGE_TEST_VECS,
};

/*
 * test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items:
 * range unions are the same bytes whatever the order of their items, hold an
 * empty range's mark, and keep a value's stored bytes as they are.
 */
void
test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items(void)
{
	volatile StdVec vecs[RANGE_TEST_VECS];
	volatile bool live[RANGE_TEST_VECS] = {0};
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 golden[39] = {1, 4, 6, 0, 0, 0, 6, 2, 128, 0, 0, 1, 1, 0, 0, 0, 4, 1, 0, 0, 0, 0, 0, 0, 6, 2,
		128, 0, 0, 10, 2, 0, 0, 0, 4, 10, 0, 0, 0};
		static const uint8 one_0[] = {'1', '.', '0'};
		static const uint8 one_00[] = {'1', '.', '0', '0'};
		static const uint8 after[] = {'a', 'f', 't', 'e', 'r'};
		static const uint8 bad[] = {1, ENCODING_SUMMARY_GIST_RANGE, 0, 0, 0, 0, 9};
		StdVec	   *v = (StdVec *) vecs;
		encoding_summary_tests_range_lower_t none = {0};
		encoding_summary_tests_range_lower_t lo;
		encoding_summary_tests_range_lower_t hi;
		uint8		le_lo[4];
		uint8		le_hi[4];
		const StdVec *items[3];
		encoding_summary_tests_value_keys_t keys[1];
		uint8	   *boxed;
		uint8		left;
		uint8		right;
		StdBytes	slices[2];
		StdBytes	rest;
		size_t		keys_len;
		size_t		expected_len = 1;
		StdBytes	expected_rest = {after, sizeof(after)};
		bool		equal;
		bool		is_err;

		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 1, le_lo);
		lo.f0 = 1;
		lo.f1 = true;
		lo.f2.ptr = le_lo;
		lo.f2.len = 4;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 5, le_hi);
		hi.f0 = 5;
		hi.f1 = false;
		hi.f2.ptr = le_hi;
		hi.f2.len = 4;
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_LB_INC, true, lo, true, hi, &v[RANGE_TEST_A]);
		live[RANGE_TEST_A] = true;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 3, le_lo);
		lo.f0 = 3;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 10, le_hi);
		hi.f0 = 10;
		hi.f1 = true;
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_LB_INC | ENCODING_SUMMARY_RANGE_UB_INC, true, lo, true, hi,
									 &v[RANGE_TEST_B]);
		live[RANGE_TEST_B] = true;
		boxed = std_box_new_uninit_layout_Checked(1, 1);
		STD_CHECK_ALIGNED(boxed, 1, 589, 42);
		STD_CHECK_NULL_DEREF(boxed, 589, 42);
		boxed[0] = ENCODING_SUMMARY_RANGE_EMPTY;
		keys[0].f0 = ENCODING_SUMMARY_GIST_RANGE;
		std_box_slice_into_vec_Checked(&keys[0].f1, boxed, 1, 1);
		v[RANGE_TEST_T0] = keys[0].f1;
		live[RANGE_TEST_T0] = true;
		encoding_summary_tests_value(keys, 1, &v[RANGE_TEST_EMPTY]);
		live[RANGE_TEST_EMPTY] = true;
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 4, le_hi);
		hi.f0 = 4;
		hi.f1 = false;
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_LB_INF, false, none, true, hi, &v[RANGE_TEST_BELOW]);
		live[RANGE_TEST_BELOW] = true;
		items[0] = &v[RANGE_TEST_A];
		items[1] = &v[RANGE_TEST_B];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 2, &v[RANGE_TEST_AB]);
		live[RANGE_TEST_AB] = true;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 1, le_lo);
		lo.f0 = 1;
		lo.f1 = true;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 10, le_hi);
		hi.f0 = 10;
		hi.f1 = true;
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_LB_INC | ENCODING_SUMMARY_RANGE_UB_INC, true, lo, true, hi,
									 &v[RANGE_TEST_T0]);
		live[RANGE_TEST_T0] = true;
		STD_ASSERT_EQ(592, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[RANGE_TEST_AB], &v[RANGE_TEST_T0]),
					  &v[RANGE_TEST_AB], encoding_summary_tests_VecU8_Debug_fmt, &v[RANGE_TEST_T0],
					  encoding_summary_tests_VecU8_Debug_fmt);
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		equal = v[RANGE_TEST_AB].len == sizeof(golden) && memcmp(v[RANGE_TEST_AB].ptr, golden, sizeof(golden)) == 0;
		STD_ASSERT_EQ(593, 9, equal, &v[RANGE_TEST_AB], encoding_summary_tests_VecU8_Debug_fmt, golden,
					  encoding_summary_tests_Array39U8_Debug_fmt);
		items[0] = &v[RANGE_TEST_B];
		items[1] = &v[RANGE_TEST_A];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 2, &v[RANGE_TEST_T0]);
		live[RANGE_TEST_T0] = true;
		STD_ASSERT_EQ(600, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[RANGE_TEST_T0], &v[RANGE_TEST_AB]),
					  &v[RANGE_TEST_T0], encoding_summary_tests_VecU8_Debug_fmt, &v[RANGE_TEST_AB],
					  encoding_summary_tests_VecU8_Debug_fmt);
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		items[0] = &v[RANGE_TEST_A];
		items[1] = &v[RANGE_TEST_EMPTY];
		items[2] = &v[RANGE_TEST_B];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 3, &v[RANGE_TEST_WITH_EMPTY]);
		live[RANGE_TEST_WITH_EMPTY] = true;
		std_index_check(2, v[RANGE_TEST_WITH_EMPTY].len, RUST_FILE, 602, 30);
		left = STD_VEC_AT(&v[RANGE_TEST_WITH_EMPTY], uint8, 2);
		right = ENCODING_SUMMARY_RANGE_LB_INC | ENCODING_SUMMARY_RANGE_UB_INC | ENCODING_SUMMARY_RANGE_CONTAIN_EMPTY;
		STD_ASSERT_EQ(602, 9, left == right, &left, std_u8_Debug_fmt, &right, std_u8_Debug_fmt);
		items[0] = &v[RANGE_TEST_WITH_EMPTY];
		items[1] = &v[RANGE_TEST_A];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 2, &v[RANGE_TEST_T0]);
		live[RANGE_TEST_T0] = true;
		STD_ASSERT_EQ(603, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[RANGE_TEST_T0], &v[RANGE_TEST_WITH_EMPTY]),
					  &v[RANGE_TEST_T0], encoding_summary_tests_VecU8_Debug_fmt, &v[RANGE_TEST_WITH_EMPTY],
					  encoding_summary_tests_VecU8_Debug_fmt);
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		items[0] = &v[RANGE_TEST_EMPTY];
		items[1] = &v[RANGE_TEST_EMPTY];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 2, &v[RANGE_TEST_T0]);
		live[RANGE_TEST_T0] = true;
		STD_ASSERT_EQ(604, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[RANGE_TEST_T0], &v[RANGE_TEST_EMPTY]),
					  &v[RANGE_TEST_T0], encoding_summary_tests_VecU8_Debug_fmt, &v[RANGE_TEST_EMPTY],
					  encoding_summary_tests_VecU8_Debug_fmt);
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		items[0] = &v[RANGE_TEST_EMPTY];
		items[1] = &v[RANGE_TEST_BELOW];
		items[2] = &v[RANGE_TEST_A];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 3, &v[RANGE_TEST_T0]);
		live[RANGE_TEST_T0] = true;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 5, le_hi);
		hi.f0 = 5;
		hi.f1 = false;
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_LB_INF | ENCODING_SUMMARY_RANGE_CONTAIN_EMPTY, false, none,
									 true, hi, &v[RANGE_TEST_T1]);
		live[RANGE_TEST_T1] = true;
		STD_ASSERT_EQ(605, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[RANGE_TEST_T0], &v[RANGE_TEST_T1]),
					  &v[RANGE_TEST_T0], encoding_summary_tests_VecU8_Debug_fmt, &v[RANGE_TEST_T1],
					  encoding_summary_tests_VecU8_Debug_fmt);
		live[RANGE_TEST_T1] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T1], 1, 1);
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 1, le_lo);
		lo.f0 = 1;
		lo.f1 = false;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 5, le_hi);
		hi.f0 = 5;
		hi.f1 = true;
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_UB_INC, true, lo, true, hi, &v[RANGE_TEST_EXCLUSIVE]);
		live[RANGE_TEST_EXCLUSIVE] = true;
		items[0] = &v[RANGE_TEST_A];
		items[1] = &v[RANGE_TEST_EXCLUSIVE];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 2, &v[RANGE_TEST_T0]);
		live[RANGE_TEST_T0] = true;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 1, le_lo);
		lo.f0 = 1;
		lo.f1 = true;
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_1(NULL, 5, le_hi);
		hi.f0 = 5;
		hi.f1 = true;
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_LB_INC | ENCODING_SUMMARY_RANGE_UB_INC, true, lo, true, hi,
									 &v[RANGE_TEST_T1]);
		live[RANGE_TEST_T1] = true;
		STD_ASSERT_EQ(610, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[RANGE_TEST_T0], &v[RANGE_TEST_T1]),
					  &v[RANGE_TEST_T0], encoding_summary_tests_VecU8_Debug_fmt, &v[RANGE_TEST_T1],
					  encoding_summary_tests_VecU8_Debug_fmt);
		live[RANGE_TEST_T1] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T1], 1, 1);
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		lo.f0 = 1;
		lo.f1 = true;
		lo.f2.ptr = one_0;
		lo.f2.len = sizeof(one_0);
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_LB_INC | ENCODING_SUMMARY_RANGE_UB_INF, true, lo, false,
									 none, &v[RANGE_TEST_X]);
		live[RANGE_TEST_X] = true;
		lo.f2.ptr = one_00;
		lo.f2.len = sizeof(one_00);
		encoding_summary_tests_range(ENCODING_SUMMARY_RANGE_LB_INC | ENCODING_SUMMARY_RANGE_UB_INF, true, lo, false,
									 none, &v[RANGE_TEST_Y]);
		live[RANGE_TEST_Y] = true;
		items[0] = &v[RANGE_TEST_X];
		items[1] = &v[RANGE_TEST_Y];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 2, &v[RANGE_TEST_T0]);
		live[RANGE_TEST_T0] = true;
		items[0] = &v[RANGE_TEST_Y];
		items[1] = &v[RANGE_TEST_X];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 2, &v[RANGE_TEST_T1]);
		live[RANGE_TEST_T1] = true;
		STD_ASSERT_EQ(618, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[RANGE_TEST_T0], &v[RANGE_TEST_T1]),
					  &v[RANGE_TEST_T0], encoding_summary_tests_VecU8_Debug_fmt, &v[RANGE_TEST_T1],
					  encoding_summary_tests_VecU8_Debug_fmt);
		live[RANGE_TEST_T1] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T1], 1, 1);
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		items[0] = &v[RANGE_TEST_X];
		items[1] = &v[RANGE_TEST_Y];
		test_encoding_summary_tests_range_unions_are_stored_as_they_are_and_whatever_the_order_of_their_items_closure_0(NULL, items, 2, &v[RANGE_TEST_T0]);
		live[RANGE_TEST_T0] = true;
		STD_ASSERT_EQ(619, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[RANGE_TEST_T0], &v[RANGE_TEST_X]),
					  &v[RANGE_TEST_T0], encoding_summary_tests_VecU8_Debug_fmt, &v[RANGE_TEST_X],
					  encoding_summary_tests_VecU8_Debug_fmt);
		live[RANGE_TEST_T0] = false;
		std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		slices[0].ptr = v[RANGE_TEST_AB].ptr;
		slices[0].len = v[RANGE_TEST_AB].len;
		slices[1] = expected_rest;
		std_slice_concat(slices, 2, sizeof(StdBytes), 1, 1, &v[RANGE_TEST_WITH_REST]);
		live[RANGE_TEST_WITH_REST] = true;
		if (!encoding_summary_gist_keys(v[RANGE_TEST_WITH_REST].ptr, v[RANGE_TEST_WITH_REST].len, &v[RANGE_TEST_KEYS],
										&rest, (ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 621, 50);
		}
		live[RANGE_TEST_KEYS] = true;
		keys_len = v[RANGE_TEST_KEYS].len;
		equal = keys_len == expected_len && rest.len == expected_rest.len &&
			memcmp(rest.ptr, expected_rest.ptr, rest.len) == 0;
		STD_ASSERT_EQ(622, 9, equal,
					  &STD_TUPLE(2, STD_ITEM(&keys_len, std_usize_Debug_fmt), STD_ITEM(&rest, std_bytes_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&expected_len, std_usize_Debug_fmt),
								 STD_ITEM(&expected_rest, std_bytes_Debug_fmt)),
					  std_tuple_Debug_fmt);
		slices[0].ptr = bad;
		slices[0].len = sizeof(bad);
		is_err = !encoding_summary_gist_union(0, slices, 1, &v[RANGE_TEST_T0], (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(&v[RANGE_TEST_T0], 1, 1);
		STD_ASSERT(623, 9, is_err, "gist_union(0, &[&[1, GIST_RANGE, 0, 0, 0, 0, 9]]).is_err()");
		for (int k = RANGE_TEST_VECS - 1; k >= 0; k--)
		{
			if (live[k])
			{
				live[k] = false;
				if (k == RANGE_TEST_KEYS)
					std_vec_free_Checked(&v[k], sizeof(encoding_summary_gist_keys_t), _Alignof(encoding_summary_gist_keys_t));
				else
					std_vec_free_Checked(&v[k], 1, 1);
			}
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		for (int k = RANGE_TEST_VECS - 1; k >= 0; k--)
		{
			if (live[k])
			{
				if (k == RANGE_TEST_KEYS)
					std_vec_free_Checked((StdVec *) &vecs[k], sizeof(encoding_summary_gist_keys_t),
										 _Alignof(encoding_summary_gist_keys_t));
				else
					std_vec_free_Checked((StdVec *) &vecs[k], 1, 1);
			}
		}
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0
 * is union: the GiST union of leaf values.
 */
static bool
test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(void *captures,
																					   const StdVec *items, size_t items_len,
																					   StdVec *result,
																					   ChunksErrorError *error)
{
	volatile StdVec slices;
	volatile bool slices_live = false;
	bool		ok = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdVec	   *slices_v = (StdVec *) &slices;
		StdSizeHint hint = {items_len, true, items_len};

		std_vec_with_capacity_trusted(slices_v, hint, sizeof(StdBytes), _Alignof(StdBytes));
		slices_live = true;
		std_vec_extend_trusted_reserve(slices_v, hint, sizeof(StdBytes), _Alignof(StdBytes));
		for (size_t i = 0, i_len = items_len; i < i_len; i++)
		{
			StdBytes	slice = {items[i].ptr, items[i].len};

			STD_VEC_AT(slices_v, StdBytes, slices_v->len) = slice;
			slices_v->len++;
		}
		ok = encoding_summary_gist_union(0, slices_v->ptr, slices_v->len, result, error);
		slices_live = false;
		std_vec_free_Checked(slices_v, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (slices_live)
			std_vec_free_Checked((StdVec *) &slices, sizeof(StdBytes), _Alignof(StdBytes));
		std_unwind_resume(&unwind);
	}
	return ok;
}

/*
 * test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_1
 * is arr: the leaf value of a text search array key of CRCs.
 */
static void
test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_1(void *captures,
																					   const int32 *crcs, size_t crcs_len,
																					   StdVec *result)
{
	volatile StdVec crcs_2;
	volatile bool crcs_2_live = false;
	volatile encoding_summary_tests_value_keys_t keys[1];
	volatile bool keys_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const uint8 head[] = {ENCODING_SUMMARY_TSVECTOR_ARRKEY, 0, 2};
		StdVec	   *crcs_2_v = (StdVec *) &crcs_2;
		encoding_summary_tests_value_keys_t *keys_v = (encoding_summary_tests_value_keys_t *) keys;
		StdSizeHint iter = {crcs_len, true, crcs_len};
		StdSizeHint hint = std_FlattenCompat_size_hint(iter, 4);
		uint8		count[4];
		StdBytes	parts[3];

		std_vec_with_capacity_trusted(crcs_2_v, hint, 1, 1);
		crcs_2_live = true;
		std_vec_extend_trusted_reserve(crcs_2_v, hint, 1, 1);
		for (size_t i = 0, i_len = crcs_len; i < i_len; i++)
		{
			uint8		bytes[4];

			std_u32_to_be_bytes((uint32) crcs[i], bytes);
			for (size_t j = 0, j_len = 4; j < j_len; j++)
			{
				STD_VEC_AT(crcs_2_v, uint8, crcs_2_v->len) = bytes[j];
				crcs_2_v->len++;
			}
		}
		std_u32_to_be_bytes(STD_DIV_FOLDED(u32, (uint32) crcs_2_v->len, 4), count);
		parts[0].ptr = head;
		parts[0].len = sizeof(head);
		parts[1].ptr = count;
		parts[1].len = 4;
		parts[2].ptr = crcs_2_v->ptr;
		parts[2].len = crcs_2_v->len;
		keys_v[0].f0 = ENCODING_SUMMARY_GIST_TSVECTOR;
		std_slice_concat(parts, 3, sizeof(StdBytes), 1, 1, &keys_v[0].f1);
		keys_live = true;
		encoding_summary_tests_value(keys_v, 1, result);
		keys_live = false;
		std_vec_free_Checked(&keys_v[0].f1, 1, 1);
		crcs_2_live = false;
		std_vec_free_Checked(crcs_2_v, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (keys_live)
			std_vec_free_Checked((StdVec *) &keys[0].f1, 1, 1);
		if (crcs_2_live)
			std_vec_free_Checked((StdVec *) &crcs_2, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_2
 * is sign: the leaf value of a two-byte text search signature key.
 */
static void
test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_2(void *captures,
																					   const uint8 *bits,
																					   StdVec *result)
{
	volatile encoding_summary_tests_value_keys_t keys[1];
	volatile bool keys_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		encoding_summary_tests_value_keys_t *keys_v = (encoding_summary_tests_value_keys_t *) keys;
		uint8	   *boxed = std_box_new_uninit_layout_Checked(5, 1);

		STD_CHECK_ALIGNED(boxed, 1, 636, 60);
		STD_CHECK_NULL_DEREF(boxed, 636, 60);
		boxed[0] = ENCODING_SUMMARY_TSVECTOR_SIGNKEY;
		boxed[1] = 0;
		boxed[2] = 2;
		boxed[3] = bits[0];
		boxed[4] = bits[1];
		keys_v[0].f0 = ENCODING_SUMMARY_GIST_TSVECTOR;
		std_box_slice_into_vec_Checked(&keys_v[0].f1, boxed, 5, 1);
		keys_live = true;
		encoding_summary_tests_value(keys_v, 1, result);
		keys_live = false;
		std_vec_free_Checked(&keys_v[0].f1, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (keys_live)
			std_vec_free_Checked((StdVec *) &keys[0].f1, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_3
 * is query: the leaf value of a text search query's signature.
 */
static void
test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_3(void *captures, uint64 sign,
																					   StdVec *result)
{
	volatile encoding_summary_tests_value_keys_t keys[1];
	volatile bool keys_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		encoding_summary_tests_value_keys_t *keys_v = (encoding_summary_tests_value_keys_t *) keys;
		uint8		be[8];

		std_u64_to_be_bytes(sign, be);
		keys_v[0].f0 = ENCODING_SUMMARY_GIST_TSQUERY;
		std_string_from_Checked(&keys_v[0].f1, (const char *) be, 8);
		keys_live = true;
		encoding_summary_tests_value(keys_v, 1, result);
		keys_live = false;
		std_vec_free_Checked(&keys_v[0].f1, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (keys_live)
			std_vec_free_Checked((StdVec *) &keys[0].f1, 1, 1);
		std_unwind_resume(&unwind);
	}
}

/* The text search test's locals, by their order of declaration, which the landing pad drops in reverse. */
enum
{
	TEXT_SEARCH_TEST_SIGNED,
	TEXT_SEARCH_TEST_ALL_TRUE,
	TEXT_SEARCH_TEST_NARROWER,
	TEXT_SEARCH_TEST_QUERY,
	TEXT_SEARCH_TEST_WIDER,
	TEXT_SEARCH_TEST_QUERIES,
	TEXT_SEARCH_TEST_VECS,
};

/*
 * test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored:
 * text search unions keep their keys' bytes as they are stored, and refuse
 * signatures of different lengths and a query beside a document.
 */
void
test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored(void)
{
	volatile StdVec vecs[TEXT_SEARCH_TEST_VECS];
	volatile bool live[TEXT_SEARCH_TEST_VECS] = {0};
	volatile encoding_summary_tests_value_keys_t key[1];
	volatile bool key_live = false;
	volatile StdVec items[2];
	volatile bool items_live[2] = {0};
	volatile StdVec unwrapped;
	volatile bool unwrapped_live = false;
	volatile StdVec expected;
	volatile bool expected_live = false;
	volatile ChunksErrorError error;
	volatile bool error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const int32 crcs_signed_0[] = {-1, 1, 17};
		static const int32 crcs_signed_1[] = {8};
		static const int32 crcs_all_true[] = {1, -1, 8};
		static const int32 crcs_one[] = {1};
		static const uint8 bits_all_true[2] = {0xfd, 0x7e};
		static const uint8 bits_none[2] = {0, 0};
		StdVec	   *v = (StdVec *) vecs;
		StdVec	   *item = (StdVec *) items;
		StdVec	   *unwrapped_v = (StdVec *) &unwrapped;
		StdVec	   *expected_v = (StdVec *) &expected;
		encoding_summary_tests_value_keys_t *key_v = (encoding_summary_tests_value_keys_t *) key;
		uint8	   *boxed;
		uint8		be[8];
		bool		is_err;

		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_1(NULL, crcs_signed_0, 3,
																							   &item[0]);
		items_live[0] = true;
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_1(NULL, crcs_signed_1, 1,
																							   &item[1]);
		items_live[1] = true;
		if (!test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																									&v[TEXT_SEARCH_TEST_SIGNED],
																									(ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 637, 61);
		}
		live[TEXT_SEARCH_TEST_SIGNED] = true;
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		boxed = std_box_new_uninit_layout_Checked(7, 1);
		STD_CHECK_ALIGNED(boxed, 1, 638, 28);
		STD_CHECK_NULL_DEREF(boxed, 638, 28);
		boxed[0] = 1;
		boxed[1] = ENCODING_SUMMARY_GIST_TSVECTOR;
		boxed[2] = ENCODING_SUMMARY_TSVECTOR_SIGNKEY;
		boxed[3] = 0;
		boxed[4] = 2;
		boxed[5] = 0x02;
		boxed[6] = 0x81;
		std_box_slice_into_vec_Checked(expected_v, boxed, 7, 1);
		expected_live = true;
		STD_ASSERT_EQ(638, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[TEXT_SEARCH_TEST_SIGNED], expected_v),
					  &v[TEXT_SEARCH_TEST_SIGNED], encoding_summary_tests_VecU8_Debug_fmt, expected_v,
					  encoding_summary_tests_VecU8_Debug_fmt);
		expected_live = false;
		std_vec_free_Checked(expected_v, 1, 1);
		std_vec_clone(&item[0], &v[TEXT_SEARCH_TEST_SIGNED], 1, 1);
		items_live[0] = true;
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_1(NULL, NULL, 0, &item[1]);
		items_live[1] = true;
		if (!test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2, unwrapped_v,
																									(ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 639, 55);
		}
		unwrapped_live = true;
		STD_ASSERT_EQ(639, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, &v[TEXT_SEARCH_TEST_SIGNED]),
					  unwrapped_v, encoding_summary_tests_VecU8_Debug_fmt, &v[TEXT_SEARCH_TEST_SIGNED],
					  encoding_summary_tests_VecU8_Debug_fmt);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		boxed = std_box_new_uninit_layout_Checked(5, 1);
		STD_CHECK_ALIGNED(boxed, 1, 640, 24);
		STD_CHECK_NULL_DEREF(boxed, 640, 24);
		boxed[0] = 1;
		boxed[1] = ENCODING_SUMMARY_GIST_TSVECTOR;
		boxed[2] = ENCODING_SUMMARY_TSVECTOR_SIGNKEY | ENCODING_SUMMARY_TSVECTOR_ALLISTRUE;
		boxed[3] = 0;
		boxed[4] = 2;
		std_box_slice_into_vec_Checked(&v[TEXT_SEARCH_TEST_ALL_TRUE], boxed, 5, 1);
		live[TEXT_SEARCH_TEST_ALL_TRUE] = true;
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_2(NULL, bits_all_true,
																							   &item[0]);
		items_live[0] = true;
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_1(NULL, crcs_all_true, 3,
																							   &item[1]);
		items_live[1] = true;
		if (!test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2, unwrapped_v,
																									(ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 641, 67);
		}
		unwrapped_live = true;
		STD_ASSERT_EQ(641, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, &v[TEXT_SEARCH_TEST_ALL_TRUE]),
					  unwrapped_v, encoding_summary_tests_VecU8_Debug_fmt, &v[TEXT_SEARCH_TEST_ALL_TRUE],
					  encoding_summary_tests_VecU8_Debug_fmt);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_2(NULL, bits_none, &item[0]);
		items_live[0] = true;
		std_vec_clone(&item[1], &v[TEXT_SEARCH_TEST_ALL_TRUE], 1, 1);
		items_live[1] = true;
		if (!test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2, unwrapped_v,
																									(ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 642, 61);
		}
		unwrapped_live = true;
		STD_ASSERT_EQ(642, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, &v[TEXT_SEARCH_TEST_ALL_TRUE]),
					  unwrapped_v, encoding_summary_tests_VecU8_Debug_fmt, &v[TEXT_SEARCH_TEST_ALL_TRUE],
					  encoding_summary_tests_VecU8_Debug_fmt);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		std_vec_clone(&item[0], &v[TEXT_SEARCH_TEST_ALL_TRUE], 1, 1);
		items_live[0] = true;
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_1(NULL, crcs_one, 1, &item[1]);
		items_live[1] = true;
		if (!test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2, unwrapped_v,
																									(ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 643, 58);
		}
		unwrapped_live = true;
		STD_ASSERT_EQ(643, 9, encoding_summary_tests_VecU8_PartialEq_eq(unwrapped_v, &v[TEXT_SEARCH_TEST_ALL_TRUE]),
					  unwrapped_v, encoding_summary_tests_VecU8_Debug_fmt, &v[TEXT_SEARCH_TEST_ALL_TRUE],
					  encoding_summary_tests_VecU8_Debug_fmt);
		unwrapped_live = false;
		std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		boxed = std_box_new_uninit_layout_Checked(3, 1);
		STD_CHECK_ALIGNED(boxed, 1, 644, 48);
		STD_CHECK_NULL_DEREF(boxed, 644, 48);
		boxed[0] = ENCODING_SUMMARY_TSVECTOR_SIGNKEY | ENCODING_SUMMARY_TSVECTOR_ALLISTRUE;
		boxed[1] = 0;
		boxed[2] = 1;
		key_v[0].f0 = ENCODING_SUMMARY_GIST_TSVECTOR;
		std_box_slice_into_vec_Checked(&key_v[0].f1, boxed, 3, 1);
		key_live = true;
		encoding_summary_tests_value(key_v, 1, &v[TEXT_SEARCH_TEST_NARROWER]);
		live[TEXT_SEARCH_TEST_NARROWER] = true;
		key_live = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		std_vec_clone(&item[0], &v[TEXT_SEARCH_TEST_ALL_TRUE], 1, 1);
		items_live[0] = true;
		std_vec_clone(&item[1], &v[TEXT_SEARCH_TEST_NARROWER], 1, 1);
		items_live[1] = true;
		is_err = !test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																										 unwrapped_v,
																										 (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		STD_ASSERT(645, 9, is_err, "union(&[all_true.clone(), narrower.clone()]).is_err()");
		item[0] = v[TEXT_SEARCH_TEST_NARROWER];
		live[TEXT_SEARCH_TEST_NARROWER] = false;
		items_live[0] = true;
		std_vec_clone(&item[1], &v[TEXT_SEARCH_TEST_ALL_TRUE], 1, 1);
		items_live[1] = true;
		is_err = !test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																										 unwrapped_v,
																										 (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		STD_ASSERT(646, 9, is_err, "union(&[narrower, all_true.clone()]).is_err()");
		std_u64_to_be_bytes(1, be);
		key_v[0].f0 = ENCODING_SUMMARY_GIST_TSQUERY;
		std_string_from_Checked(&key_v[0].f1, (const char *) be, 8);
		key_live = true;
		encoding_summary_tests_value(key_v, 1, &v[TEXT_SEARCH_TEST_QUERY]);
		live[TEXT_SEARCH_TEST_QUERY] = true;
		key_live = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		std_vec_clone(&item[0], &v[TEXT_SEARCH_TEST_ALL_TRUE], 1, 1);
		items_live[0] = true;
		std_vec_clone(&item[1], &v[TEXT_SEARCH_TEST_QUERY], 1, 1);
		items_live[1] = true;
		is_err = !test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																										 unwrapped_v,
																										 (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		STD_ASSERT(648, 9, is_err, "union(&[all_true.clone(), query.clone()]).is_err()");
		item[0] = v[TEXT_SEARCH_TEST_QUERY];
		live[TEXT_SEARCH_TEST_QUERY] = false;
		items_live[0] = true;
		std_vec_clone(&item[1], &v[TEXT_SEARCH_TEST_ALL_TRUE], 1, 1);
		items_live[1] = true;
		is_err = !test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																										 unwrapped_v,
																										 (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		STD_ASSERT(649, 9, is_err, "union(&[query, all_true.clone()]).is_err()");
		boxed = std_box_new_uninit_layout_Checked(6, 1);
		STD_CHECK_ALIGNED(boxed, 1, 650, 45);
		STD_CHECK_NULL_DEREF(boxed, 650, 45);
		boxed[0] = ENCODING_SUMMARY_TSVECTOR_SIGNKEY;
		boxed[1] = 0;
		boxed[2] = 3;
		boxed[3] = 0;
		boxed[4] = 0;
		boxed[5] = 0;
		key_v[0].f0 = ENCODING_SUMMARY_GIST_TSVECTOR;
		std_box_slice_into_vec_Checked(&key_v[0].f1, boxed, 6, 1);
		key_live = true;
		encoding_summary_tests_value(key_v, 1, &v[TEXT_SEARCH_TEST_WIDER]);
		live[TEXT_SEARCH_TEST_WIDER] = true;
		key_live = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		item[0] = v[TEXT_SEARCH_TEST_SIGNED];
		live[TEXT_SEARCH_TEST_SIGNED] = false;
		items_live[0] = true;
		item[1] = v[TEXT_SEARCH_TEST_WIDER];
		live[TEXT_SEARCH_TEST_WIDER] = false;
		items_live[1] = true;
		is_err = !test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																										 unwrapped_v,
																										 (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		STD_ASSERT(651, 9, is_err, "union(&[signed, wider]).is_err()");
		boxed = std_box_new_uninit_layout_Checked(3, 1);
		STD_CHECK_ALIGNED(boxed, 1, 652, 49);
		STD_CHECK_NULL_DEREF(boxed, 652, 49);
		boxed[0] = ENCODING_SUMMARY_TSVECTOR_SIGNKEY;
		boxed[1] = 0;
		boxed[2] = 0;
		key_v[0].f0 = ENCODING_SUMMARY_GIST_TSVECTOR;
		std_box_slice_into_vec_Checked(&key_v[0].f1, boxed, 3, 1);
		key_live = true;
		encoding_summary_tests_value(key_v, 1, &item[0]);
		items_live[0] = true;
		is_err = !test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 1,
																										 unwrapped_v,
																										 (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		key_live = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		STD_ASSERT(652, 9, is_err, "union(&[value(&[(GIST_TSVECTOR, vec![TSVECTOR_SIGNKEY, 0, 0])])]).is_err()");
		boxed = std_box_new_uninit_layout_Checked(7, 1);
		STD_CHECK_ALIGNED(boxed, 1, 653, 49);
		STD_CHECK_NULL_DEREF(boxed, 653, 49);
		boxed[0] = ENCODING_SUMMARY_TSVECTOR_ARRKEY;
		boxed[1] = 0;
		boxed[2] = 2;
		boxed[3] = 0;
		boxed[4] = 0;
		boxed[5] = 0;
		boxed[6] = 1;
		key_v[0].f0 = ENCODING_SUMMARY_GIST_TSVECTOR;
		std_box_slice_into_vec_Checked(&key_v[0].f1, boxed, 7, 1);
		key_live = true;
		encoding_summary_tests_value(key_v, 1, &item[0]);
		items_live[0] = true;
		is_err = !test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 1,
																										 unwrapped_v,
																										 (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		key_live = false;
		std_vec_free_Checked(&key_v[0].f1, 1, 1);
		STD_ASSERT(653, 9, is_err,
				   "union(&[value(&[(GIST_TSVECTOR, vec![TSVECTOR_ARRKEY, 0, 2, 0, 0, 0, 1])])]).is_err()");
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_3(NULL,
																							   STD_SHL_FOLDED(u64, 1, 3),
																							   &item[0]);
		items_live[0] = true;
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_3(NULL,
																							   STD_SHL_FOLDED(u64, 1, 63) | 1,
																							   &item[1]);
		items_live[1] = true;
		if (!test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																									&v[TEXT_SEARCH_TEST_QUERIES],
																									(ChunksErrorError *) &error))
		{
			error_live = true;
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1,
									 (ChunksErrorError *) &error, chunks_error_Error_Debug_fmt, RUST_FILE, 656, 67);
		}
		live[TEXT_SEARCH_TEST_QUERIES] = true;
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		boxed = std_box_new_uninit_layout_Checked(10, 1);
		STD_CHECK_ALIGNED(boxed, 1, 657, 29);
		STD_CHECK_NULL_DEREF(boxed, 657, 29);
		boxed[0] = 1;
		boxed[1] = ENCODING_SUMMARY_GIST_TSQUERY;
		boxed[2] = 0x80;
		boxed[3] = 0;
		boxed[4] = 0;
		boxed[5] = 0;
		boxed[6] = 0;
		boxed[7] = 0;
		boxed[8] = 0;
		boxed[9] = 0x09;
		std_box_slice_into_vec_Checked(expected_v, boxed, 10, 1);
		expected_live = true;
		STD_ASSERT_EQ(657, 9, encoding_summary_tests_VecU8_PartialEq_eq(&v[TEXT_SEARCH_TEST_QUERIES], expected_v),
					  &v[TEXT_SEARCH_TEST_QUERIES], encoding_summary_tests_VecU8_Debug_fmt, expected_v,
					  encoding_summary_tests_VecU8_Debug_fmt);
		expected_live = false;
		std_vec_free_Checked(expected_v, 1, 1);
		item[0] = v[TEXT_SEARCH_TEST_QUERIES];
		live[TEXT_SEARCH_TEST_QUERIES] = false;
		items_live[0] = true;
		test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_1(NULL, crcs_one, 1, &item[1]);
		items_live[1] = true;
		is_err = !test_encoding_summary_tests_text_search_unions_bytes_stay_as_they_are_stored_closure_0(NULL, item, 2,
																										 unwrapped_v,
																										 (ChunksErrorError *) &error);
		if (is_err)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		else
			std_vec_free_Checked(unwrapped_v, 1, 1);
		items_live[0] = false;
		std_vec_free_Checked(&item[0], 1, 1);
		items_live[1] = false;
		std_vec_free_Checked(&item[1], 1, 1);
		STD_ASSERT(658, 9, is_err, "union(&[queries, arr(&[1])]).is_err()");
		for (int k = TEXT_SEARCH_TEST_VECS - 1; k >= 0; k--)
		{
			if (live[k])
			{
				live[k] = false;
				std_vec_free_Checked(&v[k], 1, 1);
			}
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			chunks_error_Error_drop((ChunksErrorError *) &error);
		if (expected_live)
			std_vec_free_Checked((StdVec *) &expected, 1, 1);
		if (unwrapped_live)
			std_vec_free_Checked((StdVec *) &unwrapped, 1, 1);
		for (int k = 0; k < 2; k++)
		{
			if (items_live[k])
				std_vec_free_Checked((StdVec *) &items[k], 1, 1);
		}
		if (key_live)
			std_vec_free_Checked((StdVec *) &key[0].f1, 1, 1);
		for (int k = TEXT_SEARCH_TEST_VECS - 1; k >= 0; k--)
		{
			if (live[k])
				std_vec_free_Checked((StdVec *) &vecs[k], 1, 1);
		}
		std_unwind_resume(&unwind);
	}
}

#endif							/* DOLTGRES_TEST_ENCODING */
