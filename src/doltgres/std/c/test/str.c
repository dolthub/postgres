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
 * The C port of alloctests' str.rs tests of the str forms no crate's test
 * reaches (library/alloctests/tests/str.rs, Rust 1.99.1): utf8_char_counts,
 * Chars::count over every alignment, and slice_index's index_fail cases of a
 * Range, a RangeFrom, and a RangeTo, each a #[should_panic] test.
 */
#include "postgres.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_str.h"
#include "doltgres/std/vec.h"

/* RUST_FILE is str.rs, where each test's checks report. */
#define RUST_FILE STD_RUST_LIBRARY STD_SEP "alloctests" STD_SEP "tests" STD_SEP "str.rs"

/* AlloctestsStrTmpl is a utf8_char_counts template: its str and its char count. */
typedef struct AlloctestsStrTmpl
{
	StdStr		s;
	size_t		count;
} AlloctestsStrTmpl;

/*
 * alloctests_str_utf8_char_counts_check is utf8_char_counts' innermost
 * body: the template repeated between the paddings, the padding skipped, and
 * its chars counted against the template's count times the repeat.
 */
static void
alloctests_str_utf8_char_counts_check(const AlloctestsStrTmpl *tmpl, size_t repeat, const StdString *pad_start,
									  const StdString *pad_end)
{
	StdString	repeated;
	StdString	with_padding;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1), STD_FMT_ARG(2)};
	StdFmtArg	args[3];
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	size_t		si;
	size_t		ei;
	StdStr		target;
	size_t		expected_count;
	size_t		count;

	std_str_repeat(tmpl->s.ptr, tmpl->s.len, repeat, &repeated);
	args[0].value = pad_start;
	args[0].fmt = std_String_Display_fmt;
	args[1].value = &repeated;
	args[1].fmt = std_String_Display_fmt;
	args[2].value = pad_end;
	args[2].fmt = std_String_Display_fmt;
	std_fmt_format_Checked(&with_padding, &arguments);
	std_vec_free_Checked(&repeated, 1, 1);
	si = pad_start->len;
	ei = STD_SUB(usize, with_padding.len, pad_end->len, 2454, 30);
	std_str_index_range(with_padding.ptr, with_padding.len, si, ei, RUST_FILE, 2455, 35);
	target.ptr = STD_PTR_AT(with_padding.ptr, si, 1);
	target.len = std_wrapping_sub_usize(ei, si);
	STD_ASSERT(2457, 21, !std_bytes_starts_with((const uint8 *) target.ptr, target.len, (const uint8 *) " ", 1) &&
			   !std_str_ends_with_str_Checked(target.ptr, target.len, " ", 1),
			   "!target.starts_with(\" \") && !target.ends_with(\" \")");
	expected_count = STD_MUL(usize, tmpl->count, repeat, 2458, 42);
	count = std_str_Chars_count_Checked(target.ptr, target.len);
	if (expected_count != count)
	{
		size_t		pads[2] = {pad_start->len, pad_end->len};
		StdTupleRef refs = STD_TUPLE(2, STD_ITEM(&pads[0], std_usize_Debug_fmt), STD_ITEM(&pads[1], std_usize_Debug_fmt));
		StdFmtPiece mpieces[] = {STD_FMT_LIT("wrong count for `"), STD_FMT_ARG(0), STD_FMT_LIT(".repeat("),
		STD_FMT_ARG(1), STD_FMT_LIT(")` (padding: `"), STD_FMT_ARG(2), STD_FMT_LIT("`)")};
		StdFmtArg	margs[] = {{&tmpl->s, std_str_Debug_fmt}, {&repeat, std_usize_Display_fmt}, {&refs, std_tuple_Debug_fmt}};
		StdFmtArguments message = {mpieces, lengthof(mpieces), margs};

		std_assert_failed(RUST_FILE, 2459, 21, "==", &expected_count, std_usize_Debug_fmt, &count, std_usize_Debug_fmt,
						  &message);
	}
	std_vec_free_Checked(&with_padding, 1, 1);
}

/* test_alloctests_str_utf8_char_counts is str.rs' utf8_char_counts, outside Miri. */
void
test_alloctests_str_utf8_char_counts(void)
{
	static const AlloctestsStrTmpl strs[] = {
		{{"e", 1}, 1}, {{"\xc3\xa9", 2}, 1}, {{"\xe2\x82\xac", 3}, 1}, {{"\xf0\x90\x80\x80", 4}, 1},
		{{"e\xc3\xa9\xe2\x82\xac\xf0\x90\x80\x80", 10}, 4},
	};
	static const size_t short_reps[] = {8, 64, 256, 512};
	static const size_t long_reps[] = {1024, 1 << 16};
	size_t		spread = 8;
	StdVec		reps;
	StdVec		padding;

	std_vec_new(&reps, sizeof(size_t), _Alignof(size_t));
	for (size_t i = 0, i_len = lengthof(short_reps); i < i_len; i++)
	{
		size_t		n = short_reps[i];

		for (size_t r = STD_SUB(usize, n, spread, 2434, 23), r_end = STD_ADD(usize, n, spread, 2434, 36);; r = std_forward_unchecked_usize_Checked(r, 1))
		{
			std_vec_push_Checked(&reps, &r, sizeof(size_t), _Alignof(size_t));
			if (r == r_end)
				break;
		}
	}
	for (size_t i = 0, i_len = lengthof(long_reps); i < i_len; i++)
	{
		size_t		n = long_reps[i];

		for (size_t r = STD_SUB(usize, n, spread, 2438, 67), r_end = STD_ADD(usize, n, spread, 2438, 80);; r = std_forward_unchecked_usize_Checked(r, 1))
		{
			std_vec_push_Checked(&reps, &r, sizeof(size_t), _Alignof(size_t));
			if (r == r_end)
				break;
		}
	}
	std_vec_new(&padding, sizeof(StdString), _Alignof(StdString));
	for (size_t len = 0, len_end = 8; len < len_end; len = std_forward_unchecked_usize_Checked(len, 1))
	{
		StdString	pad;

		std_str_repeat(" ", 1, len, &pad);
		std_vec_push_Checked(&padding, &pad, sizeof(StdString), _Alignof(StdString));
	}
	for (size_t i = 0, i_len = reps.len; i < i_len; i++)
		for (size_t t = 0, t_len = lengthof(strs); t < t_len; t++)
			for (size_t a = 0, a_len = padding.len; a < a_len; a++)
				for (size_t b = 0, b_len = padding.len; b < b_len; b++)
					alloctests_str_utf8_char_counts_check(&strs[t], STD_VEC_AT(&reps, size_t, i),
														  &STD_VEC_AT(&padding, StdString, a),
														  &STD_VEC_AT(&padding, StdString, b));
	for (size_t a = 0, a_len = padding.len; a < a_len; a++)
		std_vec_free_Checked(&STD_VEC_AT(&padding, StdString, a), 1, 1);
	std_vec_free_Checked(&padding, sizeof(StdString), _Alignof(StdString));
	std_vec_free_Checked(&reps, sizeof(size_t), _Alignof(size_t));
}

/* test_alloctests_str_slice_index_rangefrom_len_index_fail is slice_index::rangefrom_len::index_fail: &v[7..] of "abcdef"'s String, which panics. */
void
test_alloctests_str_slice_index_rangefrom_len_index_fail(void)
{
	StdString	v;

	std_string_from_Checked(&v, "abcdef", 6);
	std_str_index_range_from((const char *) v.ptr, v.len, 7, RUST_FILE, 563, 5);
	std_vec_free_Checked(&v, 1, 1);
}

/* test_alloctests_str_slice_index_rangeto_len_index_fail is slice_index::rangeto_len::index_fail: &v[..7] of "abcdef"'s String, which panics. */
void
test_alloctests_str_slice_index_rangeto_len_index_fail(void)
{
	StdString	v;

	std_string_from_Checked(&v, "abcdef", 6);
	std_str_index_range_to((const char *) v.ptr, v.len, 7, RUST_FILE, 563, 5);
	std_vec_free_Checked(&v, 1, 1);
}

/* test_alloctests_str_slice_index_range_len_len_index_fail is slice_index::range_len_len::index_fail: &v[7..7] of "abcdef"'s String, which panics. */
void
test_alloctests_str_slice_index_range_len_len_index_fail(void)
{
	StdString	v;

	std_string_from_Checked(&v, "abcdef", 6);
	std_str_index_range((const char *) v.ptr, v.len, 7, 7, RUST_FILE, 563, 5);
	std_vec_free_Checked(&v, 1, 1);
}

/* test_alloctests_str_slice_index_range_neg_width_index_fail is slice_index::range_neg_width::index_fail: &v[4..3] of "abcdef"'s String, which panics. */
void
test_alloctests_str_slice_index_range_neg_width_index_fail(void)
{
	StdString	v;

	std_string_from_Checked(&v, "abcdef", 6);
	std_str_index_range((const char *) v.ptr, v.len, 4, 3, RUST_FILE, 629, 5);
	std_vec_free_Checked(&v, 1, 1);
}

/* test_alloctests_str_slice_index_boundary_range_1_index_fail is slice_index::boundary::range_1::index_fail: &v[4..7] of "abc\xce\xb1\xce\xb2\xce\xb3"'s String, which panics. */
void
test_alloctests_str_slice_index_boundary_range_1_index_fail(void)
{
	StdString	v;

	std_string_from_Checked(&v, "abc\xce\xb1\xce\xb2\xce\xb3", 9);
	std_str_index_range((const char *) v.ptr, v.len, 4, 7, RUST_FILE, 676, 9);
	std_vec_free_Checked(&v, 1, 1);
}

/* test_alloctests_str_slice_index_boundary_range_2_index_fail is slice_index::boundary::range_2::index_fail: &v[3..6] of "abc\xce\xb1\xce\xb2\xce\xb3"'s String, which panics. */
void
test_alloctests_str_slice_index_boundary_range_2_index_fail(void)
{
	StdString	v;

	std_string_from_Checked(&v, "abc\xce\xb1\xce\xb2\xce\xb3", 9);
	std_str_index_range((const char *) v.ptr, v.len, 3, 6, RUST_FILE, 676, 9);
	std_vec_free_Checked(&v, 1, 1);
}

/* test_alloctests_str_slice_index_boundary_rangefrom_index_fail is slice_index::boundary::rangefrom::index_fail: &v[4..] of "abc\xce\xb1\xce\xb2\xce\xb3"'s String, which panics. */
void
test_alloctests_str_slice_index_boundary_rangefrom_index_fail(void)
{
	StdString	v;

	std_string_from_Checked(&v, "abc\xce\xb1\xce\xb2\xce\xb3", 9);
	std_str_index_range_from((const char *) v.ptr, v.len, 4, RUST_FILE, 676, 9);
	std_vec_free_Checked(&v, 1, 1);
}

/* test_alloctests_str_slice_index_boundary_rangeto_index_fail is slice_index::boundary::rangeto::index_fail: &v[..6] of "abc\xce\xb1\xce\xb2\xce\xb3"'s String, which panics. */
void
test_alloctests_str_slice_index_boundary_rangeto_index_fail(void)
{
	StdString	v;

	std_string_from_Checked(&v, "abc\xce\xb1\xce\xb2\xce\xb3", 9);
	std_str_index_range_to((const char *) v.ptr, v.len, 6, RUST_FILE, 676, 9);
	std_vec_free_Checked(&v, 1, 1);
}
