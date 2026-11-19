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
 * The template of core's sort tests' tests.rs (library/alloctests/tests/sort,
 * Rust 1.99.1) at an element type, run against the stable sort:
 * check_is_sorted, which checks the sort's result against
 * known_good_stable_sort's on a failure, test_is_sorted, and, at the three
 * types gen_sort_test_fns_with_default_patterns_3_ty! names, the generic
 * tests it runs. An instance defines its
 * parameters and includes this file; every parameter is undefined at the
 * end. STD_ST_TS spells T; STD_ST_UNIT, when defined, makes T () and leaves
 * its storage out; otherwise STD_ST_T is T, STD_ST_LT is its PartialOrd::lt
 * on pointers, STD_ST_CLONE(dst, src) its Clone into dst, STD_ST_DROP its
 * drop glue, left undefined for a T with none, STD_ST_DEBUG its Debug, and
 * STD_ST_IN_PLACE 1 when T has i32's size and alignment, so that a mapped
 * Vec<i32> collects into its own buffer as in_place_collect does.
 * STD_ST_PAIR_TS, defined at the three types, spells (T, i32), whose
 * struct STD_ST_PAIR_T holds T in f0 and the i32 in f1, and turns on the
 * generic tests, which take STD_ST_CMP, T's Ord::cmp on pointers, and
 * STD_ST_EQ, its PartialEq::eq on pointers.
 * The includer instantiates the stable sort, known_good_stable_sort, and
 * the slice template at the same STD_ST_TS first, with the stable
 * instantiation's Sort::name and, at the three types, its Sort::sort_by and
 * the sort at (T, i32), and defines RUST_FILE as tests.rs, where its checks
 * report.
 */

#include "doltgres/std/env.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/io.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/std/test_sort.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* STD_ST_CAT pastes two tokens after expanding them. */
#define STD_ST_CAT_(a, b) a##b
#define STD_ST_CAT(a, b) STD_ST_CAT_(a, b)

/* STD_ST_NAME names the instance's functions. */
#define STD_ST_NAME(name) STD_ST_CAT(alloctests_sort_tests_##name##_, STD_ST_TS)

/* STD_ST_SORT_NAME is the stable instantiation's Sort::name. */
#define STD_ST_SORT_NAME "rust_std_stable"

/* STD_ST_ELEM_SIZE is T's size, which a slice's iterators step by, and none for (). */
#ifndef STD_ST_UNIT
#define STD_ST_ELEM_SIZE sizeof(STD_ST_T)
#else
#define STD_ST_ELEM_SIZE 0
#endif

#ifndef STD_ST_UNIT

/* The instance's Vec<T> Debug, its slice's: a list of the elements. */
static bool
STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _Debug_fmt)) (const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_DebugList_entry(&list, STD_PTR_AT(self->ptr, k, sizeof(STD_ST_T)), STD_ST_DEBUG);
	return std_DebugList_finish(&list);
}

/* The instance's Vec<T> drop glue: each element's, then the buffer. */
static void
STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (StdVec *self)
{
#ifdef STD_ST_DROP
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		STD_ST_DROP(STD_PTR_AT(self->ptr, k, sizeof(STD_ST_T)));
#endif
	std_vec_free_Checked(self, sizeof(STD_ST_T), _Alignof(STD_ST_T));
}

/* The instance's to_vec is <[T]>::to_vec: a vector of each element cloned in order, the clones so far dropped should one panic. */
static void
STD_ST_NAME(to_vec) (const STD_ST_T * v, size_t len, StdVec *result)
{
	std_vec_with_capacity_Checked(result, len, sizeof(STD_ST_T), _Alignof(STD_ST_T));
	for (size_t k = 0, k_len = len; k < k_len; k++)
	{
		STD_ST_CLONE((STD_ST_T *) STD_PTR_AT(result->ptr, k, sizeof(STD_ST_T)), &v[k]);
		result->len = std_wrapping_add_usize(result->len, 1);
	}
}

#endif							/* STD_ST_UNIT */

/* The instance's check_is_sorted frame is its state across its pad: what it holds while live. */
typedef struct STD_ST_NAME (check_is_sorted_frame_0_t)
{
#ifndef STD_ST_UNIT
	STD_ST_T   *v;
#endif
	size_t		len;
	StdVec		v_orig;
	bool		v_orig_live;
	StdVec		known_good_sorted_vec;
	bool		known_good_sorted_vec_live;
	StdString	names[3];
	bool		names_live[3];
	StdString text;
	bool		text_live;
} STD_ST_NAME(check_is_sorted_frame_0_t);

/* The instance's check_is_sorted body. */
static void
STD_ST_NAME(check_is_sorted_body_0) (void *frame_)
{
	STD_ST_NAME(check_is_sorted_frame_0_t) * frame = frame_;
#ifndef STD_ST_UNIT
	uint64		seed = alloctests_sort_patterns_get_or_init_rand_seed();
	bool		is_small_test = frame->len <= 100;
#endif
	size_t		left;
	size_t		right;
	StdWindows	windows;
	const void *window_;
	size_t		window_len;

#ifndef STD_ST_UNIT
	STD_ST_NAME(to_vec) (frame->v, frame->len, &frame->v_orig);
#else
	alloctests_sort_patterns_get_or_init_rand_seed();
	std_vec_new(&frame->v_orig, 0, 1);
	frame->v_orig.len = frame->len;
#endif
	frame->v_orig_live = true;
#ifndef STD_ST_UNIT
	STD_ST_CAT(std_slice_sort_, STD_ST_TS) (frame->v, frame->len);
#else
	STD_ST_CAT(std_slice_sort_, STD_ST_TS) (frame->len);
#endif
	left = frame->len;
	right = frame->v_orig.len;
	STD_ASSERT_EQ(35, 5, left == right, &left, std_usize_Debug_fmt, &right,
				  std_usize_Debug_fmt);
#ifndef STD_ST_UNIT
	windows = std_slice_windows(frame->v, frame->len, 2, RUST_FILE, 37, 21);
#else
	windows = std_slice_windows(STD_DANGLING, frame->len, 2, RUST_FILE, 37, 21);
#endif
	while (std_slice_Windows_next(&windows, STD_ST_ELEM_SIZE, &window_, &window_len))
	{
#ifndef STD_ST_UNIT
		const		STD_ST_T *window = window_;

#endif
		std_index_check(0, window_len, RUST_FILE, 38, 12);
		std_index_check(1, window_len, RUST_FILE, 38, 24);
#ifndef STD_ST_UNIT
		if (!STD_ST_LT(&window[1], &window[0]))
			continue;
		STD_ST_NAME(to_vec) (frame->v_orig.ptr, frame->v_orig.len, &frame->known_good_sorted_vec);
		frame->known_good_sorted_vec_live = true;
		STD_ST_CAT(alloctests_sort_known_good_stable_sort_sort_, STD_ST_TS) (frame->known_good_sorted_vec.ptr,
																			 frame->known_good_sorted_vec.len);
		if (is_small_test)
		{
			StdVec		got = {frame->v, frame->len, frame->len};
			static const StdFmtPiece original[] = {STD_FMT_LIT("Original: "), STD_FMT_ARG(0), STD_FMT_LIT("\n")};
			static const StdFmtPiece expected[] = {STD_FMT_LIT("Expected: "), STD_FMT_ARG(0), STD_FMT_LIT("\n")};
			static const StdFmtPiece got_pieces[] = {STD_FMT_LIT("Got:      "), STD_FMT_ARG(0), STD_FMT_LIT("\n")};
			StdFmtArg	original_args[] = {{&frame->v_orig, STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _Debug_fmt))}};
			StdFmtArg	expected_args[] = {{&frame->known_good_sorted_vec, STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _Debug_fmt))}};
			StdFmtArg	got_args[] = {{&got, STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _Debug_fmt))}};
			StdFmtArguments original_arguments = {original, lengthof(original), original_args};
			StdFmtArguments expected_arguments = {expected, lengthof(expected), expected_args};
			StdFmtArguments got_arguments = {got_pieces, lengthof(got_pieces), got_args};

			std_io__eprint(&original_arguments);
			std_io__eprint(&expected_arguments);
			std_io__eprint(&got_arguments);
		}
		else
		{
			StdString	var;
			StdVarError var_error;
			bool		set = std_env_var("WRITE_LARGE_FAILURE", &var, &var_error);

			if (set)
				std_String_drop(&var);
			else
				std_VarError_drop(&var_error);
			if (set)
			{
				static const StdFmtPiece original[] = {STD_FMT_LIT("original_"), STD_FMT_ARG(0), STD_FMT_LIT(".txt")};
				static const StdFmtPiece known[] = {STD_FMT_LIT("known_good_sorted_"), STD_FMT_ARG(0), STD_FMT_LIT(".txt")};
				static const StdFmtPiece tested[] = {STD_FMT_ARG(0), STD_FMT_LIT("_sorted_"), STD_FMT_ARG(1), STD_FMT_LIT(".txt")};
				StdStr		sort_name = {STD_ST_SORT_NAME, sizeof(STD_ST_SORT_NAME) - 1};
				StdFmtArg	seed_args[] = {{&seed, std_u64_Display_fmt}};
				StdFmtArg	tested_args[] = {{&sort_name, std_str_Display_fmt}, {&seed, std_u64_Display_fmt}};
				StdFmtArguments original_arguments = {original, lengthof(original), seed_args};
				StdFmtArguments known_arguments = {known, lengthof(known), seed_args};
				StdFmtArguments tested_arguments = {tested, lengthof(tested), tested_args};
				const StdVec *contents[3];
				StdVec		got = {frame->v, frame->len, frame->len};
				static const StdFmtPiece failed[] = {STD_FMT_LIT("Failed comparison, see files "), STD_FMT_ARG(0),
				STD_FMT_LIT(", "), STD_FMT_ARG(1), STD_FMT_LIT(", and "), STD_FMT_ARG(2), STD_FMT_LIT("\n")};
				StdFmtArg	failed_args[3];
				StdFmtArguments failed_arguments = {failed, lengthof(failed), failed_args};

				std_fmt_format_Checked(&frame->names[0], &original_arguments);
				frame->names_live[0] = true;
				std_fmt_format_Checked(&frame->names[1], &known_arguments);
				frame->names_live[1] = true;
				std_fmt_format_Checked(&frame->names[2], &tested_arguments);
				frame->names_live[2] = true;
				contents[0] = &frame->v_orig;
				contents[1] = &frame->known_good_sorted_vec;
				contents[2] = &got;
				for (size_t n = 0, n_len = 3; n < n_len; n++)
				{
					static const uint32 write_lines[3] = {53, 54, 55};
					static const uint32 write_cols[3] = {72, 82, 67};
					static const StdFmtPiece debug[] = {STD_FMT_ARG(0)};
					StdFmtArg	debug_args[] = {{contents[n], STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _Debug_fmt))}};
					StdFmtArguments debug_arguments = {debug, lengthof(debug), debug_args};
					StdIoError	error;

					std_fmt_format_Checked(&frame->text, &debug_arguments);
					frame->text_live = true;
					if (!std_fs_write(frame->names[n].ptr, frame->names[n].len, frame->text.ptr,
									  frame->text.len, &error))
					{
						static const char text[] = "called `Result::unwrap()` on an `Err` value";

						std_result_unwrap_failed(text, sizeof(text) - 1, &error, std_io_Error_Debug_fmt, RUST_FILE,
												 write_lines[n], write_cols[n]);
					}
					frame->text_live = false;
					std_String_drop(&frame->text);
				}
				for (size_t n = 0, n_len = 3; n < n_len; n++)
				{
					failed_args[n].value = &frame->names[n];
					failed_args[n].fmt = std_String_Display_fmt;
				}
				std_io__eprint(&failed_arguments);
				for (size_t n = 3; n > 0; n--)
				{
					frame->names_live[n - 1] = false;
					std_String_drop(&frame->names[n - 1]);
				}
			}
			else
			{
				static const StdFmtPiece rerun[] = {STD_FMT_LIT("Failed comparison, re-run with WRITE_LARGE_FAILURE env var set, to get output.\n")};
				StdFmtArguments rerun_arguments = {rerun, lengthof(rerun), NULL};

				std_io__eprint(&rerun_arguments);
			}
		}
		{
			static const char text[] = "Test assertion failed!";

			std_panic(RUST_FILE, 67, 13, text, sizeof(text) - 1);
		}
#endif
	}
	frame->v_orig_live = false;
#ifndef STD_ST_UNIT
	STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->v_orig);
#endif
}

/* The instance's check_is_sorted pad: what it holds, in reverse order, while live. */
static void
STD_ST_NAME(check_is_sorted_pad_0) (void *frame_)
{
	STD_ST_NAME(check_is_sorted_frame_0_t) * frame = frame_;

	if (frame->text_live)
		std_String_drop(&frame->text);
	for (size_t n = 3; n > 0; n--)
		if (frame->names_live[n - 1])
			std_String_drop(&frame->names[n - 1]);
#ifndef STD_ST_UNIT
	if (frame->known_good_sorted_vec_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->known_good_sorted_vec);
	if (frame->v_orig_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->v_orig);
#endif
}

/*
 * The instance's check_is_sorted sorts v and checks the result is sorted,
 * printing the input, known_good_stable_sort's result, and the sort's on a
 * failure, into files for a large input when WRITE_LARGE_FAILURE is set.
 */
#ifndef STD_ST_UNIT
static void
STD_ST_NAME(check_is_sorted) (STD_ST_T * v, size_t len)
{
	STD_ST_NAME(check_is_sorted_frame_0_t) frame =
	{
		.v = v, .len = len
	};

	std_landing(&frame, STD_ST_NAME(check_is_sorted_body_0), STD_ST_NAME(check_is_sorted_pad_0));
}
#else
static void
STD_ST_NAME(check_is_sorted) (size_t len)
{
	STD_ST_NAME(check_is_sorted_frame_0_t) frame =
	{
		.len = len
	};

	std_landing(&frame, STD_ST_NAME(check_is_sorted_body_0), STD_ST_NAME(check_is_sorted_pad_0));
}
#endif

#ifndef STD_ST_UNIT

/* The instance's collect_mapped frame is its state across its pad: the pattern it consumes, the map, and the vector it fills, while live. */
typedef struct STD_ST_NAME (collect_mapped_frame_0_t)
{
	StdVec		pattern;
	size_t		pattern_at;
	bool		pattern_live;
	void		(*map_fn) (void *captures, int32 val, STD_ST_T * result);
	void	   *map_fn_captures;
	void		(*map_fn_drop) (void *);
	bool		map_fn_live;
	StdVec	   *result;
	bool		result_live;
} STD_ST_NAME(collect_mapped_frame_0_t);

/*
 * The instance's collect_mapped body: the pattern's IntoIter mapped into T,
 * collected as Vec's from_iter collects it, in place when T has i32's size
 * and alignment, the map dropped with the iterator after.
 */
static void
STD_ST_NAME(collect_mapped_body_0) (void *frame_)
{
	STD_ST_NAME(collect_mapped_frame_0_t) * frame = frame_;

#if STD_ST_IN_PLACE
	frame->result->ptr = frame->pattern.ptr;
	frame->result->cap = frame->pattern.cap;
	frame->result->len = 0;
	frame->result_live = true;
	for (frame->pattern_at = 0; frame->pattern_at < frame->pattern.len;)
	{
		int32		val = STD_VEC_AT(&frame->pattern, int32, frame->pattern_at);
		STD_ST_T	mapped;

		frame->pattern_at = std_wrapping_add_usize(frame->pattern_at, 1);
		frame->map_fn(frame->map_fn_captures, val, &mapped);
		memcpy(STD_PTR_AT(frame->result->ptr, frame->result->len, sizeof(STD_ST_T)), &mapped, sizeof(STD_ST_T));
		frame->result->len = std_wrapping_add_usize(frame->result->len, 1);
	}
	frame->pattern_live = false;
#else
	{
		StdSizeHint hint = {frame->pattern.len, true, frame->pattern.len};

		std_vec_with_capacity_trusted(frame->result, hint, sizeof(STD_ST_T), _Alignof(STD_ST_T));
		frame->result_live = true;
		std_vec_extend_trusted_reserve(frame->result, hint, sizeof(STD_ST_T), _Alignof(STD_ST_T));
	}
	for (frame->pattern_at = 0; frame->pattern_at < frame->pattern.len;)
	{
		int32		val = STD_VEC_AT(&frame->pattern, int32, frame->pattern_at);
		STD_ST_T	mapped;

		frame->pattern_at = std_wrapping_add_usize(frame->pattern_at, 1);
		frame->map_fn(frame->map_fn_captures, val, &mapped);
		memcpy(STD_PTR_AT(frame->result->ptr, frame->result->len, sizeof(STD_ST_T)), &mapped, sizeof(STD_ST_T));
		frame->result->len = std_wrapping_add_usize(frame->result->len, 1);
	}
	frame->pattern_live = false;
	std_vec_free_Checked(&frame->pattern, sizeof(int32), _Alignof(int32));
#endif
	frame->map_fn_live = false;
	if (frame->map_fn_drop != NULL)
		frame->map_fn_drop(frame->map_fn_captures);
	frame->result_live = false;
}

/* The instance's collect_mapped pad: the vector so far, the pattern's rest, and the map, while live. */
static void
STD_ST_NAME(collect_mapped_pad_0) (void *frame_)
{
	STD_ST_NAME(collect_mapped_frame_0_t) * frame = frame_;

#if STD_ST_IN_PLACE
	if (frame->result_live)
	{
#ifdef STD_ST_DROP
		for (size_t k = 0, k_len = frame->result->len; k < k_len; k++)
			STD_ST_DROP(STD_PTR_AT(frame->result->ptr, k, sizeof(STD_ST_T)));
#endif
		std_vec_free_Checked(frame->result, sizeof(STD_ST_T), _Alignof(STD_ST_T));
	}
#else
	if (frame->result_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (frame->result);
	if (frame->pattern_live)
		std_vec_free_Checked(&frame->pattern, sizeof(int32), _Alignof(int32));
#endif
	if (frame->map_fn_live && frame->map_fn_drop != NULL)
		frame->map_fn_drop(frame->map_fn_captures);
}

/*
 * The instance's collect_mapped is pattern.into_iter().map(map_fn).collect():
 * it takes the pattern and the map, and fills result.
 */
static void
STD_ST_NAME(collect_mapped) (StdVec *pattern, void (*map_fn) (void *captures, int32 val, STD_ST_T * result),
							 void *map_fn_captures, void (*map_fn_drop) (void *), StdVec *result)
{
	STD_ST_NAME(collect_mapped_frame_0_t) frame =
	{
		.pattern = *pattern, .pattern_live = true, .map_fn = map_fn,
			.map_fn_captures = map_fn_captures, .map_fn_drop = map_fn_drop, .map_fn_live = true, .result = result
	};

	std_landing(&frame, STD_ST_NAME(collect_mapped_body_0), STD_ST_NAME(collect_mapped_pad_0));
}

/* The instance's test_is_sorted frame is its state across its pad: its functions, the pattern, and the test data, while live. */
typedef struct STD_ST_NAME (test_is_sorted_frame_0_t)
{
	size_t		test_len;
	void		(*map_fn) (void *captures, int32 val, STD_ST_T * result);
	void	   *map_fn_captures;
	void		(*map_fn_drop) (void *);
	bool		map_fn_live;
	void		(*pattern_fn) (void *captures, size_t len, StdVec *result);
	void	   *pattern_fn_captures;
	void		(*pattern_fn_drop) (void *);
	bool		pattern_fn_live;
	StdVec		pattern;
	StdVec		test_data;
	bool		test_data_live;
} STD_ST_NAME(test_is_sorted_frame_0_t);

/* The instance's test_is_sorted body: the pattern mapped into T and collected, then checked. */
static void
STD_ST_NAME(test_is_sorted_body_0) (void *frame_)
{
	STD_ST_NAME(test_is_sorted_frame_0_t) * frame = frame_;

	frame->pattern_fn(frame->pattern_fn_captures, frame->test_len, &frame->pattern);
	frame->map_fn_live = false;
	STD_ST_NAME(collect_mapped) (&frame->pattern, frame->map_fn, frame->map_fn_captures, frame->map_fn_drop,
								 &frame->test_data);
	frame->test_data_live = true;
	STD_ST_NAME(check_is_sorted) (frame->test_data.ptr, frame->test_data.len);
	frame->test_data_live = false;
	STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_data);
	frame->pattern_fn_live = false;
	if (frame->pattern_fn_drop != NULL)
		frame->pattern_fn_drop(frame->pattern_fn_captures);
}

/* The instance's test_is_sorted pad: the test data and the functions, while live. */
static void
STD_ST_NAME(test_is_sorted_pad_0) (void *frame_)
{
	STD_ST_NAME(test_is_sorted_frame_0_t) * frame = frame_;

	if (frame->test_data_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_data);
	if (frame->pattern_fn_live && frame->pattern_fn_drop != NULL)
		frame->pattern_fn_drop(frame->pattern_fn_captures);
	if (frame->map_fn_live && frame->map_fn_drop != NULL)
		frame->map_fn_drop(frame->map_fn_captures);
}

/* The instance's test_is_sorted checks the sort of the pattern of the length mapped into T, taking both functions. */
static void
STD_ST_NAME(test_is_sorted) (size_t test_len, void (*map_fn) (void *captures, int32 val, STD_ST_T * result),
							 void *map_fn_captures, void (*map_fn_drop) (void *),
							 void (*pattern_fn) (void *captures, size_t len, StdVec *result),
							 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	STD_ST_NAME(test_is_sorted_frame_0_t) frame =
	{
		.test_len = test_len, .map_fn = map_fn,
			.map_fn_captures = map_fn_captures, .map_fn_drop = map_fn_drop, .map_fn_live = true,
			.pattern_fn = pattern_fn, .pattern_fn_captures = pattern_fn_captures, .pattern_fn_drop = pattern_fn_drop,
			.pattern_fn_live = true
	};

	std_landing(&frame, STD_ST_NAME(test_is_sorted_body_0), STD_ST_NAME(test_is_sorted_pad_0));
}

#ifdef STD_ST_PAIR_TS
/* The instance's sum_from is the i64 sum of each element's type_from_fn, Sum's fold with its overflow check. */
static int64
STD_ST_NAME(sum_from) (const StdVec *v, int32 (*type_from_fn) (void *captures, const STD_ST_T * val),
					   void *type_from_fn_captures)
{
	int64		sum = 0;

	for (size_t k = 0, k_len = v->len; k < k_len; k++)
		sum = std_add_i64(sum, (int64) type_from_fn(type_from_fn_captures, &STD_VEC_AT(v, STD_ST_T, k)),
						  STD_ACCUM_FILE, 206, 1);
	return sum;
}

/* The instance's pair Vec<(T, i32)> drop glue: each pair's T, then the buffer. */
static void
STD_ST_NAME(pair_vec_drop) (StdVec *self)
{
#ifdef STD_ST_DROP
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		STD_ST_DROP(&STD_VEC_AT(self, STD_ST_PAIR_T, k).f0);
#endif
	std_vec_free_Checked(self, sizeof(STD_ST_PAIR_T), _Alignof(STD_ST_PAIR_T));
}

/* The instance's pair_to_vec is <[(T, i32)]>::to_vec: a vector of each pair cloned in order. */
static void
STD_ST_NAME(pair_to_vec) (const STD_ST_PAIR_T * v, size_t len, StdVec *result)
{
	std_vec_with_capacity_Checked(result, len, sizeof(STD_ST_PAIR_T), _Alignof(STD_ST_PAIR_T));
	for (size_t k = 0, k_len = len; k < k_len; k++)
	{
		STD_ST_PAIR_T *dst = STD_PTR_AT(result->ptr, k, sizeof(STD_ST_PAIR_T));

		STD_ST_CLONE(&dst->f0, &v[k].f0);
		dst->f1 = v[k].f1;
		result->len = std_wrapping_add_usize(result->len, 1);
	}
}

/* The instance's pair_le is (T, i32)'s PartialOrd::le: lexical_ord's chain, T's partial_cmp then i32's le. */
static bool
STD_ST_NAME(pair_le) (const STD_ST_PAIR_T * a, const STD_ST_PAIR_T * b)
{
	StdOrdering c = STD_ST_CMP(&a->f0, &b->f0);

	return c != STD_ORDERING_EQUAL ? c == STD_ORDERING_LESS : a->f1 <= b->f1;
}

/* The instance's stability_with_patterns frame is its state across its pad: the functions it takes, Sort::name's String, the pattern, the pair being made, and the two vectors, while live. */
typedef struct STD_ST_NAME (stability_with_patterns_frame_0_t)
{
	size_t		len;
	void		(*type_into_fn) (void *captures, int32 val, STD_ST_T * result);
	void	   *type_into_fn_captures;
	void		(*type_into_fn_drop) (void *);
	void	   *type_from_fn_captures;
	void		(*type_from_fn_drop) (void *);
	bool		fns_live;
	void		(*pattern_fn) (size_t len, StdVec *result);
	StdString	name;
	bool		name_live;
	StdVec		pattern;
	bool		pattern_live;
	STD_ST_T	first;
	bool		first_live;
	StdVec		orig;
	bool		orig_live;
	StdVec		v;
	bool		v_live;
} STD_ST_NAME(stability_with_patterns_frame_0_t);

/* The instance's stability_with_patterns body: pattern values tagged with their occurrence sort into tag order. */
static void
STD_ST_NAME(stability_with_patterns_body_0) (void *frame_)
{
	STD_ST_NAME(stability_with_patterns_frame_0_t) * frame = frame_;
	static const char unstable[] = "unstable";
	bool		skip;
	int32		counts[128] = {0};
	StdWindows	windows;
	const void *window;
	size_t		window_len;
	bool		all = true;

	alloctests_sort_tests_stable_SortImpl_name(&frame->name);
	frame->name_live = true;
	skip = std_str_contains_str_Checked(frame->name.ptr, frame->name.len, unstable, sizeof(unstable) - 1);
	frame->name_live = false;
	std_String_drop(&frame->name);
	if (skip)
	{
		frame->fns_live = false;
		if (frame->type_from_fn_drop != NULL)
			frame->type_from_fn_drop(frame->type_from_fn_captures);
		if (frame->type_into_fn_drop != NULL)
			frame->type_into_fn_drop(frame->type_into_fn_captures);
		return;
	}
	frame->pattern_fn(frame->len, &frame->pattern);
	frame->pattern_live = true;
	{
		StdSizeHint hint = {frame->pattern.len, true, frame->pattern.len};

		std_vec_with_capacity_trusted(&frame->orig, hint, sizeof(STD_ST_PAIR_T), _Alignof(STD_ST_PAIR_T));
		frame->orig_live = true;
		std_vec_extend_trusted_reserve(&frame->orig, hint, sizeof(STD_ST_PAIR_T), _Alignof(STD_ST_PAIR_T));
	}
	for (size_t k = 0, k_len = frame->pattern.len; k < k_len; k++)
	{
		int32		val = STD_VEC_AT(&frame->pattern, int32, k);
		int32		n = STD_REM(i32, std_saturating_abs_i32(val), (int32) lengthof(counts), 545, 21);
		STD_ST_PAIR_T pair;

		std_index_check((size_t) n, lengthof(counts), RUST_FILE, 546, 13);
		counts[(size_t) n] = STD_ADD(i32, counts[(size_t) n], 1, 546, 13);
		frame->type_into_fn(frame->type_into_fn_captures, n, &frame->first);
		frame->first_live = true;
		std_index_check((size_t) n, lengthof(counts), RUST_FILE, 547, 31);
		frame->first_live = false;
		pair.f0 = frame->first;
		pair.f1 = counts[(size_t) n];
		memcpy(STD_PTR_AT(frame->orig.ptr, frame->orig.len, sizeof(STD_ST_PAIR_T)), &pair, sizeof(STD_ST_PAIR_T));
		frame->orig.len = std_wrapping_add_usize(frame->orig.len, 1);
	}
	STD_ST_NAME(pair_to_vec) (frame->orig.ptr, frame->orig.len, &frame->v);
	frame->v_live = true;
	STD_ST_CAT(std_slice_sort_, STD_ST_PAIR_TS) (frame->v.ptr, frame->v.len);
	windows = std_slice_windows(frame->v.ptr, frame->v.len, 2, RUST_FILE, 561, 15);
	while (std_slice_Windows_next(&windows, sizeof(STD_ST_PAIR_T), &window, &window_len))
	{
		const		STD_ST_PAIR_T *w = window;

		std_index_check(0, window_len, RUST_FILE, 561, 34);
		std_index_check(1, window_len, RUST_FILE, 561, 42);
		if (!STD_ST_NAME(pair_le) (&w[0], &w[1]))
		{
			all = false;
			break;
		}
	}
	STD_ASSERT(561, 5, all, "v.windows(2).all(|w| w[0] <= w[1])");
	frame->v_live = false;
	STD_ST_NAME(pair_vec_drop) (&frame->v);
	frame->orig_live = false;
	STD_ST_NAME(pair_vec_drop) (&frame->orig);
	frame->pattern_live = false;
	std_vec_free_Checked(&frame->pattern, sizeof(int32), _Alignof(int32));
	frame->fns_live = false;
	if (frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
	if (frame->type_into_fn_drop != NULL)
		frame->type_into_fn_drop(frame->type_into_fn_captures);
}

/* The instance's stability_with_patterns pad: its String, pair, and vectors, while live. */
static void
STD_ST_NAME(stability_with_patterns_pad_0) (void *frame_)
{
	STD_ST_NAME(stability_with_patterns_frame_0_t) * frame = frame_;

	if (frame->v_live)
		STD_ST_NAME(pair_vec_drop) (&frame->v);
#ifdef STD_ST_DROP
	if (frame->first_live)
		STD_ST_DROP(&frame->first);
#endif
	if (frame->orig_live)
		STD_ST_NAME(pair_vec_drop) (&frame->orig);
	if (frame->pattern_live)
		std_vec_free_Checked(&frame->pattern, sizeof(int32), _Alignof(int32));
	if (frame->name_live)
		std_String_drop(&frame->name);
	if (frame->fns_live)
	{
		if (frame->type_from_fn_drop != NULL)
			frame->type_from_fn_drop(frame->type_from_fn_captures);
		if (frame->type_into_fn_drop != NULL)
			frame->type_into_fn_drop(frame->type_into_fn_captures);
	}
}

/* The instance's stability_with_patterns checks that the stable sort keeps equal elements in order, taking both functions. */
static void
STD_ST_NAME(stability_with_patterns) (size_t len, void (*type_into_fn) (void *captures, int32 val, STD_ST_T * result),
									  void *type_into_fn_captures, void (*type_into_fn_drop) (void *),
									  int32 (*type_from_fn) (void *captures, const STD_ST_T * val),
									  void *type_from_fn_captures, void (*type_from_fn_drop) (void *),
									  void (*pattern_fn) (size_t len, StdVec *result))
{
	STD_ST_NAME(stability_with_patterns_frame_0_t) frame =
	{
		.len = len, .type_into_fn = type_into_fn,
			.type_into_fn_captures = type_into_fn_captures, .type_into_fn_drop = type_into_fn_drop,
			.type_from_fn_captures = type_from_fn_captures, .type_from_fn_drop = type_from_fn_drop, .fns_live = true,
			.pattern_fn = pattern_fn
	};

	std_landing(&frame, STD_ST_NAME(stability_with_patterns_body_0), STD_ST_NAME(stability_with_patterns_pad_0));
}

/* The instance's calc_comps_required closure's captures: the counter and the comparison, by reference. */
typedef struct STD_ST_NAME (calc_comps_required_closure_0_t)
{
	uint32	   *comp_counter;
	StdOrdering (*cmp_fn) (void *captures, const STD_ST_T * a, const STD_ST_T * b);
	void	   *cmp_fn_captures;
} STD_ST_NAME(calc_comps_required_closure_0_t);

/* The instance's calc_comps_required closure: one more comparison counted, then cmp_fn's. */
static StdOrdering
STD_ST_NAME(calc_comps_required_closure_0) (void *captures, const void *a, const void *b)
{
	STD_ST_NAME(calc_comps_required_closure_0_t) * closure = captures;

	*closure->comp_counter = STD_ADD(u32, *closure->comp_counter, 1, 183, 9);
	return closure->cmp_fn(closure->cmp_fn_captures, a, b);
}

/* The instance's calc_comps_required frame is its state across its pad: the comparison, while live. */
typedef struct STD_ST_NAME (calc_comps_required_frame_0_t)
{
	STD_ST_T   *v;
	size_t		len;
	StdOrdering (*cmp_fn) (void *captures, const STD_ST_T * a, const STD_ST_T * b);
	void	   *cmp_fn_captures;
	void		(*cmp_fn_drop) (void *);
	bool		cmp_fn_live;
	uint32		comp_counter;
} STD_ST_NAME(calc_comps_required_frame_0_t);

/* The instance's calc_comps_required body: the comparisons a sort_by makes with cmp_fn, counted. */
static void
STD_ST_NAME(calc_comps_required_body_0) (void *frame_)
{
	STD_ST_NAME(calc_comps_required_frame_0_t) * frame = frame_;
	STD_ST_NAME(calc_comps_required_closure_0_t) closure =
	{
		&frame->comp_counter, frame->cmp_fn,
			frame->cmp_fn_captures
	};

	STD_ST_CAT(alloctests_sort_tests_stable_SortImpl_sort_by_, STD_ST_TS) (frame->v, frame->len,
																		   STD_ST_NAME(calc_comps_required_closure_0),
																		   &closure, NULL);
	frame->cmp_fn_live = false;
	if (frame->cmp_fn_drop != NULL)
		frame->cmp_fn_drop(frame->cmp_fn_captures);
}

/* The instance's calc_comps_required pad: the comparison, while live. */
static void
STD_ST_NAME(calc_comps_required_pad_0) (void *frame_)
{
	STD_ST_NAME(calc_comps_required_frame_0_t) * frame = frame_;

	if (frame->cmp_fn_live && frame->cmp_fn_drop != NULL)
		frame->cmp_fn_drop(frame->cmp_fn_captures);
}

/* The instance's calc_comps_required is the number of comparisons the stable sort_by makes on v, taking cmp_fn. */
static uint32
STD_ST_NAME(calc_comps_required) (STD_ST_T * v, size_t len,
								  StdOrdering (*cmp_fn) (void *captures, const STD_ST_T * a, const STD_ST_T * b),
								  void *cmp_fn_captures, void (*cmp_fn_drop) (void *))
{
	STD_ST_NAME(calc_comps_required_frame_0_t) frame =
	{
		.v = v, .len = len, .cmp_fn = cmp_fn,
			.cmp_fn_captures = cmp_fn_captures, .cmp_fn_drop = cmp_fn_drop, .cmp_fn_live = true
	};

	std_landing(&frame, STD_ST_NAME(calc_comps_required_body_0), STD_ST_NAME(calc_comps_required_pad_0));
	return frame.comp_counter;
}

/* The instance's Ord::cmp as the FnMut(&T, &T) -> Ordering a test passes, |a, b| a.cmp(b). */
static StdOrdering
STD_ST_NAME(cmp_closure) (void *captures, const STD_ST_T * a, const STD_ST_T * b)
{
	return STD_ST_CMP(a, b);
}

/* The instance's panic_retain_orig_set closure_1's captures: the counter and the threshold, by reference. */
typedef struct STD_ST_NAME (panic_retain_orig_set_closure_1_t)
{
	size_t	   *comp_counter;
	const size_t *panic_threshold;
} STD_ST_NAME(panic_retain_orig_set_closure_1_t);

/* The instance's panic_retain_orig_set closure_1: a panic at the threshold's comparison, the others a.cmp(b) and counted. */
static StdOrdering
STD_ST_NAME(panic_retain_orig_set_closure_1) (void *captures, const void *a, const void *b)
{
	STD_ST_NAME(panic_retain_orig_set_closure_1_t) * closure = captures;

	if (*closure->comp_counter == *closure->panic_threshold)
		STD_PANIC(622, 17, "explicit panic");
	*closure->comp_counter = STD_ADD(usize, *closure->comp_counter, 1, 624, 13);
	return STD_ST_CMP((const STD_ST_T *) a, (const STD_ST_T *) b);
}

/* The instance's panic_retain_orig_set closure_0's captures: the test data, the counter, and the threshold, by reference. */
typedef struct STD_ST_NAME (panic_retain_orig_set_closure_0_t)
{
	StdVec	   *test_data;
	size_t	   *comp_counter;
	const size_t *panic_threshold;
} STD_ST_NAME(panic_retain_orig_set_closure_0_t);

/* The instance's panic_retain_orig_set closure_0, catch_unwind's body: the test data's sort_by with closure_1. */
static void
STD_ST_NAME(panic_retain_orig_set_closure_0) (void *captures)
{
	STD_ST_NAME(panic_retain_orig_set_closure_0_t) * closure = captures;
	STD_ST_NAME(panic_retain_orig_set_closure_1_t) inner =
	{
		closure->comp_counter, closure->panic_threshold
	};

	STD_ST_CAT(alloctests_sort_tests_stable_SortImpl_sort_by_, STD_ST_TS) (closure->test_data->ptr,
																		   closure->test_data->len,
																		   STD_ST_NAME(panic_retain_orig_set_closure_1),
																		   &inner, NULL);
}

/* The instance's panic_retain_orig_set frame is its state across its pad: the pattern, the test data, the clone, the random value, and the caught panic, while live. */
typedef struct STD_ST_NAME (panic_retain_orig_set_frame_0_t)
{
	size_t		len;
	void		(*type_into_fn) (void *captures, int32 val, STD_ST_T * result);
	void	   *type_into_fn_captures;
	int32		(*type_from_fn) (void *captures, const STD_ST_T * val);
	void	   *type_from_fn_captures;
	void		(*type_from_fn_drop) (void *);
	bool		type_from_fn_live;
	void		(*pattern_fn) (size_t len, StdVec *result);
	StdVec		pattern;
	StdVec		test_data;
	bool		test_data_live;
	StdVec		clone;
	bool		clone_live;
	StdVec		random;
	bool		random_live;
	StdCaught	res;
	bool		res_live;
} STD_ST_NAME(panic_retain_orig_set_frame_0_t);

/* The instance's panic_retain_orig_set body: a sort_by whose comparison panics at a random one leaves the same elements. */
static void
STD_ST_NAME(panic_retain_orig_set_body_0) (void *frame_)
{
	STD_ST_NAME(panic_retain_orig_set_frame_0_t) * frame = frame_;
	int64		sum_before;
	int64		sum_after;
	uint32		required_comps;
	size_t		panic_threshold;
	size_t		comp_counter = 0;
	bool		returned;

	frame->pattern_fn(frame->len, &frame->pattern);
	STD_ST_NAME(collect_mapped) (&frame->pattern, frame->type_into_fn, frame->type_into_fn_captures, NULL,
								 &frame->test_data);
	frame->test_data_live = true;
	sum_before = STD_ST_NAME(sum_from) (&frame->test_data, frame->type_from_fn, frame->type_from_fn_captures);
	STD_ST_NAME(to_vec) (frame->test_data.ptr, frame->test_data.len, &frame->clone);
	frame->clone_live = true;
	required_comps = STD_ST_NAME(calc_comps_required) (frame->clone.ptr, frame->clone.len, STD_ST_NAME(cmp_closure),
													   NULL, NULL);
	frame->clone_live = false;
	STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->clone);
	alloctests_sort_patterns_random_uniform_RangeInclusiveI32(1, 1, (int32) required_comps, &frame->random);
	frame->random_live = true;
	std_index_check(0, frame->random.len, RUST_FILE, 612, 81);
	panic_threshold = STD_SUB(usize, (size_t) STD_VEC_AT(&frame->random, int32, 0), 1,
							  612, 27);
	frame->random_live = false;
	std_vec_free_Checked(&frame->random, sizeof(int32), _Alignof(int32));
	{
		STD_ST_NAME(panic_retain_orig_set_closure_0_t) closure =
		{
			&frame->test_data, &comp_counter, &panic_threshold
		};

		returned = std_catch_unwind(STD_ST_NAME(panic_retain_orig_set_closure_0), &closure, &frame->res);
	}
	frame->res_live = !returned;
	STD_ASSERT(630, 5, !returned, "res.is_err()");
	sum_after = STD_ST_NAME(sum_from) (&frame->test_data, frame->type_from_fn, frame->type_from_fn_captures);
	STD_ASSERT_EQ(635, 5, sum_before == sum_after, &sum_before, std_i64_Debug_fmt, &sum_after,
				  std_i64_Debug_fmt);
	if (frame->res_live)
	{
		frame->res_live = false;
		std_Caught_drop_Checked(&frame->res);
	}
	frame->test_data_live = false;
	STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_data);
	frame->type_from_fn_live = false;
	if (frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
}

/* The instance's panic_retain_orig_set pad: the caught panic, the vectors, and type_from_fn, while live. */
static void
STD_ST_NAME(panic_retain_orig_set_pad_0) (void *frame_)
{
	STD_ST_NAME(panic_retain_orig_set_frame_0_t) * frame = frame_;

	if (frame->res_live)
		std_Caught_drop_Checked(&frame->res);
	if (frame->random_live)
		std_vec_free_Checked(&frame->random, sizeof(int32), _Alignof(int32));
	if (frame->clone_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->clone);
	if (frame->test_data_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_data);
	if (frame->type_from_fn_live && frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
}

/* The instance's panic_retain_orig_set checks that a panicking comparison leaves the elements' set, taking type_from_fn. */
static void
STD_ST_NAME(panic_retain_orig_set) (size_t len, void (*type_into_fn) (void *captures, int32 val, STD_ST_T * result),
									void *type_into_fn_captures,
									int32 (*type_from_fn) (void *captures, const STD_ST_T * val),
									void *type_from_fn_captures, void (*type_from_fn_drop) (void *),
									void (*pattern_fn) (size_t len, StdVec *result))
{
	STD_ST_NAME(panic_retain_orig_set_frame_0_t) frame =
	{
		.len = len, .type_into_fn = type_into_fn,
			.type_into_fn_captures = type_into_fn_captures, .type_from_fn = type_from_fn,
			.type_from_fn_captures = type_from_fn_captures, .type_from_fn_drop = type_from_fn_drop,
			.type_from_fn_live = true, .pattern_fn = pattern_fn
	};

	std_landing(&frame, STD_ST_NAME(panic_retain_orig_set_body_0), STD_ST_NAME(panic_retain_orig_set_pad_0));
}


/* The instance's map closure's captures: the Copy type_into_fn, by reference. */
typedef struct STD_ST_NAME (into_closure_t)
{
	void		(*type_into_fn) (void *captures, int32 val, STD_ST_T * result);
	void	   *type_into_fn_captures;
} STD_ST_NAME(into_closure_t);

/* The instance's into_closure is |val| type_into_fn(val), the map deterministic and self_cmp collect with. */
static void
STD_ST_NAME(into_closure) (void *captures, int32 val, STD_ST_T * result)
{
	STD_ST_NAME(into_closure_t) * closure = captures;

	closure->type_into_fn(closure->type_into_fn_captures, val, result);
}

/* The instance's deterministic closure_1's captures: type_from_fn, by reference. */
typedef struct STD_ST_NAME (deterministic_closure_1_t)
{
	int32		(*type_from_fn) (void *captures, const STD_ST_T * val);
	void	   *type_from_fn_captures;
} STD_ST_NAME(deterministic_closure_1_t);

/* The instance's deterministic closure_1, comparison_fn: the values' i32s compared modulo 10_000. */
static StdOrdering
STD_ST_NAME(deterministic_closure_1) (void *captures, const void *a, const void *b)
{
	STD_ST_NAME(deterministic_closure_1_t) * closure = captures;
	int32		a_i32 = closure->type_from_fn(closure->type_from_fn_captures, a);
	int32		b_i32 = closure->type_from_fn(closure->type_from_fn_captures, b);
	int32		a_i32_key_space_reduced = STD_REM_FOLDED(i32, a_i32, 10000);
	int32		b_i32_key_space_reduced = STD_REM_FOLDED(i32, b_i32, 10000);

	return std_i32_Ord_cmp(&a_i32_key_space_reduced, &b_i32_key_space_reduced);
}

/* The instance's Vec<T> PartialEq, its slices': the same length and equal elements. */
static bool
STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _PartialEq_eq)) (const StdVec *self, const StdVec *other)
{
	if (self->len != other->len)
		return false;
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		if (!STD_ST_EQ(&STD_VEC_AT(self, STD_ST_T, k), &STD_VEC_AT(other, STD_ST_T, k)))
			return false;
	return true;
}

/* The instance's deterministic frame is its state across its pad: type_from_fn and the two vectors, while live. */
typedef struct STD_ST_NAME (deterministic_frame_0_t)
{
	size_t		len;
	void		(*type_into_fn) (void *captures, int32 val, STD_ST_T * result);
	void	   *type_into_fn_captures;
	int32		(*type_from_fn) (void *captures, const STD_ST_T * val);
	void	   *type_from_fn_captures;
	void		(*type_from_fn_drop) (void *);
	bool		type_from_fn_live;
	void		(*pattern_fn) (size_t len, StdVec *result);
	StdVec		pattern;
	StdVec		test_input;
	bool		test_input_live;
	StdVec		test_input_clone;
	bool		test_input_clone_live;
} STD_ST_NAME(deterministic_frame_0_t);

/* The instance's deterministic body: two sort_bys of the same input on a reduced key give the same order. */
static void
STD_ST_NAME(deterministic_body_0) (void *frame_)
{
	STD_ST_NAME(deterministic_frame_0_t) * frame = frame_;
	STD_ST_NAME(into_closure_t) into =
	{
		frame->type_into_fn, frame->type_into_fn_captures
	};
	STD_ST_NAME(deterministic_closure_1_t) comparison_fn =
	{
		frame->type_from_fn, frame->type_from_fn_captures
	};

	frame->pattern_fn(frame->len, &frame->pattern);
	STD_ST_NAME(collect_mapped) (&frame->pattern, STD_ST_NAME(into_closure), &into, NULL, &frame->test_input);
	frame->test_input_live = true;
	STD_ST_NAME(to_vec) (frame->test_input.ptr, frame->test_input.len, &frame->test_input_clone);
	frame->test_input_clone_live = true;
	STD_ST_CAT(alloctests_sort_tests_stable_SortImpl_sort_by_, STD_ST_TS) (frame->test_input.ptr,
																		   frame->test_input.len,
																		   STD_ST_NAME(deterministic_closure_1),
																		   &comparison_fn, NULL);
	STD_ST_CAT(alloctests_sort_tests_stable_SortImpl_sort_by_, STD_ST_TS) (frame->test_input_clone.ptr,
																		   frame->test_input_clone.len,
																		   STD_ST_NAME(deterministic_closure_1),
																		   &comparison_fn, NULL);
	STD_ASSERT_EQ(731, 5,
				  STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _PartialEq_eq)) (&frame->test_input,
																							   &frame->test_input_clone),
				  &frame->test_input, STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _Debug_fmt)),
				  &frame->test_input_clone, STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _Debug_fmt)));
	frame->test_input_clone_live = false;
	STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_input_clone);
	frame->test_input_live = false;
	STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_input);
	frame->type_from_fn_live = false;
	if (frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
}

/* The instance's deterministic pad: the vectors and type_from_fn, while live. */
static void
STD_ST_NAME(deterministic_pad_0) (void *frame_)
{
	STD_ST_NAME(deterministic_frame_0_t) * frame = frame_;

	if (frame->test_input_clone_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_input_clone);
	if (frame->test_input_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_input);
	if (frame->type_from_fn_live && frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
}

/* The instance's deterministic checks that sort_by's order of equal keys is the same each run, taking type_from_fn. */
static void
STD_ST_NAME(deterministic) (size_t len, void (*type_into_fn) (void *captures, int32 val, STD_ST_T * result),
							void *type_into_fn_captures, int32 (*type_from_fn) (void *captures, const STD_ST_T * val),
							void *type_from_fn_captures, void (*type_from_fn_drop) (void *),
							void (*pattern_fn) (size_t len, StdVec *result))
{
	STD_ST_NAME(deterministic_frame_0_t) frame =
	{
		.len = len, .type_into_fn = type_into_fn,
			.type_into_fn_captures = type_into_fn_captures, .type_from_fn = type_from_fn,
			.type_from_fn_captures = type_from_fn_captures, .type_from_fn_drop = type_from_fn_drop,
			.type_from_fn_live = true, .pattern_fn = pattern_fn
	};

	std_landing(&frame, STD_ST_NAME(deterministic_body_0), STD_ST_NAME(deterministic_pad_0));
}

/* The instance's self_cmp closure_1, comparison_fn: a and b never the same element, then a.cmp(b). */
static StdOrdering
STD_ST_NAME(self_cmp_closure_1) (void *captures, const void *a, const void *b)
{
	if (a == b)
		std_assert_failed(RUST_FILE, 749, 9, "!=", &a, std_ptr_Debug_fmt, &b, std_ptr_Debug_fmt, NULL);
	return STD_ST_CMP((const STD_ST_T *) a, (const STD_ST_T *) b);
}

/* The instance's self_cmp frame is its state across its pad: _type_from_fn and the test input, while live. */
typedef struct STD_ST_NAME (self_cmp_frame_0_t)
{
	size_t		len;
	void		(*type_into_fn) (void *captures, int32 val, STD_ST_T * result);
	void	   *type_into_fn_captures;
	void	   *type_from_fn_captures;
	void		(*type_from_fn_drop) (void *);
	bool		type_from_fn_live;
	void		(*pattern_fn) (size_t len, StdVec *result);
	StdVec		pattern;
	StdVec		test_input;
	bool		test_input_live;
} STD_ST_NAME(self_cmp_frame_0_t);

/* The instance's self_cmp body: sort_by never compares an element with itself, and sorts. */
static void
STD_ST_NAME(self_cmp_body_0) (void *frame_)
{
	STD_ST_NAME(self_cmp_frame_0_t) * frame = frame_;
	STD_ST_NAME(into_closure_t) into =
	{
		frame->type_into_fn, frame->type_into_fn_captures
	};
	StdWindows	windows;
	const void *window_;
	size_t		window_len;

	frame->pattern_fn(frame->len, &frame->pattern);
	STD_ST_NAME(collect_mapped) (&frame->pattern, STD_ST_NAME(into_closure), &into, NULL, &frame->test_input);
	frame->test_input_live = true;
	STD_ST_CAT(alloctests_sort_tests_stable_SortImpl_sort_by_, STD_ST_TS) (frame->test_input.ptr,
																		   frame->test_input.len,
																		   STD_ST_NAME(self_cmp_closure_1), NULL,
																		   NULL);
	windows = std_slice_windows(frame->test_input.ptr, frame->test_input.len, 2, RUST_FILE, 756, 30);
	while (std_slice_Windows_next(&windows, sizeof(STD_ST_T), &window_, &window_len))
	{
		const		STD_ST_T *window = window_;

		std_index_check(0, window_len, RUST_FILE, 757, 17);
		std_index_check(1, window_len, RUST_FILE, 757, 30);
		STD_ASSERT(757, 9, STD_ST_CMP(&window[0], &window[1]) != STD_ORDERING_GREATER,
				   "window[0] <= window[1]");
	}
	frame->test_input_live = false;
	STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_input);
	frame->type_from_fn_live = false;
	if (frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
}

/* The instance's self_cmp pad: the test input and _type_from_fn, while live. */
static void
STD_ST_NAME(self_cmp_pad_0) (void *frame_)
{
	STD_ST_NAME(self_cmp_frame_0_t) * frame = frame_;

	if (frame->test_input_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_input);
	if (frame->type_from_fn_live && frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
}

/* The instance's self_cmp checks that sort_by never passes one element as both operands, taking _type_from_fn. */
static void
STD_ST_NAME(self_cmp) (size_t len, void (*type_into_fn) (void *captures, int32 val, STD_ST_T * result),
					   void *type_into_fn_captures, int32 (*type_from_fn) (void *captures, const STD_ST_T * val),
					   void *type_from_fn_captures, void (*type_from_fn_drop) (void *),
					   void (*pattern_fn) (size_t len, StdVec *result))
{
	STD_ST_NAME(self_cmp_frame_0_t) frame =
	{
		.len = len, .type_into_fn = type_into_fn,
			.type_into_fn_captures = type_into_fn_captures, .type_from_fn_captures = type_from_fn_captures,
			.type_from_fn_drop = type_from_fn_drop, .type_from_fn_live = true, .pattern_fn = pattern_fn
	};

	std_landing(&frame, STD_ST_NAME(self_cmp_body_0), STD_ST_NAME(self_cmp_pad_0));
}

/*
 * The instance's Box<dyn FnMut(&T, &T) -> Ordering>: the closure's call
 * through a shared borrow of its captures, the captures, and their drop
 * glue, size, and alignment from its vtable.
 */
typedef struct STD_ST_NAME (dyn_cmp_t)
{
	StdOrdering (*call_mut) (void *captures, const void *a, const void *b);
	void	   *captures;
	void		(*drop) (void *captures);
	size_t		size;
	size_t		align;
} STD_ST_NAME(dyn_cmp_t);

/* The instance's Box<dyn FnMut> drop glue: its captures dropped, then freed. */
static void
STD_ST_NAME(dyn_cmp_drop) (STD_ST_NAME(dyn_cmp_t) * self)
{
	if (self->drop != NULL)
		self->drop(self->captures);
	if (self->size != 0)
		std_global_dealloc(self->captures, self->size, self->align);
}

/* The instance's Box::new of a closure: its captures moved into a block of their size, none for none. */
static STD_ST_NAME(dyn_cmp_t)
STD_ST_NAME(dyn_cmp_new) (StdOrdering (*call_mut) (void *captures, const void *a, const void *b),
						  const void *captures, size_t size, size_t align)
{
	STD_ST_NAME(dyn_cmp_t) boxed =
	{
		call_mut, STD_DANGLING, NULL, size, align
	};

	if (size != 0)
	{
		boxed.captures = std_box_new_uninit_Checked(size, align);
		memcpy(boxed.captures, captures, size);
	}
	return boxed;
}

/* The instance's violate_ord_retain_orig_set closure_0's captures: the random orderings, by reference. */
typedef struct STD_ST_NAME (violate_ord_retain_orig_set_closure_0_t)
{
	const StdVec *random_orderings;
} STD_ST_NAME(violate_ord_retain_orig_set_closure_0_t);

/* The instance's violate_ord_retain_orig_set closure_0, get_random_0_1_or_2: the ordering at an index, which it advances and wraps. */
static size_t
STD_ST_NAME(violate_ord_retain_orig_set_closure_0) (const STD_ST_NAME(violate_ord_retain_orig_set_closure_0_t) * closure,
													size_t *random_idx)
{
	size_t		ridx = *random_idx;

	*random_idx = STD_ADD(usize, *random_idx, 1, 784, 9);
	if (STD_ADD(usize, ridx, 1, 785, 12) == closure->random_orderings->len)
		*random_idx = 0;
	std_index_check(ridx, closure->random_orderings->len, RUST_FILE, 789, 25);
	return (size_t) STD_VEC_AT(closure->random_orderings, int32, ridx);
}

/* The instance's violate_ord_retain_orig_set closure_1's captures: get_random_0_1_or_2 and its index, by reference. */
typedef struct STD_ST_NAME (violate_ord_retain_orig_set_closure_1_t)
{
	const		STD_ST_NAME(violate_ord_retain_orig_set_closure_0_t) * get_random_0_1_or_2;
	size_t	   *random_idx_a;
} STD_ST_NAME(violate_ord_retain_orig_set_closure_1_t);

/* The instance's violate_ord_retain_orig_set closure_1: a random ordering. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_1) (void *captures, const void *_a, const void *_b)
{
	STD_ST_NAME(violate_ord_retain_orig_set_closure_1_t) * closure = captures;
	static const StdOrdering orderings[] = {STD_ORDERING_LESS, STD_ORDERING_EQUAL, STD_ORDERING_GREATER};
	size_t		idx = STD_ST_NAME(violate_ord_retain_orig_set_closure_0) (closure->get_random_0_1_or_2,
																		  closure->random_idx_a);

	std_index_check(idx, lengthof(orderings), RUST_FILE, 813, 13);
	return orderings[idx];
}

/* The instance's violate_ord_retain_orig_set closure_2: everything is less. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_2) (void *captures, const void *_a, const void *_b)
{
	return STD_ORDERING_LESS;
}

/* The instance's violate_ord_retain_orig_set closure_3: everything is equal. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_3) (void *captures, const void *_a, const void *_b)
{
	return STD_ORDERING_EQUAL;
}

/* The instance's violate_ord_retain_orig_set closure_4: everything is greater. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_4) (void *captures, const void *_a, const void *_b)
{
	return STD_ORDERING_GREATER;
}

/* The instance's violate_ord_retain_orig_set closure_5: equal means less, else greater. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_5) (void *captures, const void *a, const void *b)
{
	return STD_ST_EQ((const STD_ST_T *) a, (const STD_ST_T *) b) ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
}

/* The instance's violate_ord_retain_orig_set closure_6's captures: the last elements and type_from_fn, by reference. */
typedef struct STD_ST_NAME (violate_ord_retain_orig_set_closure_6_t)
{
	int32	   *last_element_a;
	int32	   *last_element_b;
	int32		(*type_from_fn) (void *captures, const STD_ST_T * val);
	void	   *type_from_fn_captures;
} STD_ST_NAME(violate_ord_retain_orig_set_closure_6_t);

/* The instance's violate_ord_retain_orig_set closure_6, the transitive breaker: a pair after one sharing only its first value compares reversed. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_6) (void *captures, const void *a, const void *b)
{
	STD_ST_NAME(violate_ord_retain_orig_set_closure_6_t) * closure = captures;
	int32		lea = *closure->last_element_a;
	int32		leb = *closure->last_element_b;
	int32		a_as_i32 = closure->type_from_fn(closure->type_from_fn_captures, a);
	int32		b_as_i32 = closure->type_from_fn(closure->type_from_fn_captures, b);

	*closure->last_element_a = a_as_i32;
	*closure->last_element_b = b_as_i32;
	if (a_as_i32 == lea && b_as_i32 != leb)
		return STD_ST_CMP((const STD_ST_T *) b, (const STD_ST_T *) a);
	return STD_ST_CMP((const STD_ST_T *) a, (const STD_ST_T *) b);
}

/* The instance's violate_ord_retain_orig_set closure_7's captures: the counter, get_random_0_1_or_2, and its index, by reference. */
typedef struct STD_ST_NAME (violate_ord_retain_orig_set_closure_7_t)
{
	size_t	   *rand_counter;
	const		STD_ST_NAME(violate_ord_retain_orig_set_closure_0_t) * get_random_0_1_or_2;
	size_t	   *random_idx;
} STD_ST_NAME(violate_ord_retain_orig_set_closure_7_t);

/* The instance's violate_ord_retain_orig_set closure_7: a sampled random 1% of comparisons reversed. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_7) (void *captures, const void *a, const void *b)
{
	STD_ST_NAME(violate_ord_retain_orig_set_closure_7_t) * closure = captures;
	size_t		r = STD_ST_NAME(violate_ord_retain_orig_set_closure_0) (closure->get_random_0_1_or_2,
																		closure->random_idx);

	*closure->rand_counter = STD_ADD(usize, *closure->rand_counter, r, 847, 13);
	if (*closure->rand_counter >= 100)
	{
		*closure->rand_counter = 0;
		return STD_ST_CMP((const STD_ST_T *) b, (const STD_ST_T *) a);
	}
	return STD_ST_CMP((const STD_ST_T *) a, (const STD_ST_T *) b);
}

/* The instance's violate_ord_retain_orig_set closure_8: a sampled random 33% of comparisons reversed. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_8) (void *captures, const void *a, const void *b)
{
	STD_ST_NAME(violate_ord_retain_orig_set_closure_7_t) * closure = captures;
	size_t		r = STD_ST_NAME(violate_ord_retain_orig_set_closure_0) (closure->get_random_0_1_or_2,
																		closure->random_idx);

	*closure->rand_counter = STD_ADD(usize, *closure->rand_counter, r, 857, 13);
	if (*closure->rand_counter >= 3)
	{
		*closure->rand_counter = 0;
		return STD_ST_CMP((const STD_ST_T *) b, (const STD_ST_T *) a);
	}
	return STD_ST_CMP((const STD_ST_T *) a, (const STD_ST_T *) b);
}

/* The instance's violate_ord_retain_orig_set closure_9's captures: the streak counter, by reference. */
typedef struct STD_ST_NAME (violate_ord_retain_orig_set_closure_9_t)
{
	size_t	   *streak_counter;
} STD_ST_NAME(violate_ord_retain_orig_set_closure_9_t);

/* The instance's violate_ord_retain_orig_set closure_9: STREAK_LEN comparisons a.cmp(b), then STREAK_LEN less. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_9) (void *captures, const void *a, const void *b)
{
	STD_ST_NAME(violate_ord_retain_orig_set_closure_9_t) * closure = captures;

	*closure->streak_counter = STD_ADD(usize, *closure->streak_counter, 1, 873, 13);
	if (*closure->streak_counter <= 50)
		return STD_ST_CMP((const STD_ST_T *) a, (const STD_ST_T *) b);
	if (*closure->streak_counter == 50 * 2)
		*closure->streak_counter = 0;
	return STD_ORDERING_LESS;
}

/* The instance's violate_ord_retain_orig_set closure_10: STREAK_LEN comparisons a.cmp(b), then STREAK_LEN greater. */
static StdOrdering
STD_ST_NAME(violate_ord_retain_orig_set_closure_10) (void *captures, const void *a, const void *b)
{
	STD_ST_NAME(violate_ord_retain_orig_set_closure_9_t) * closure = captures;

	*closure->streak_counter = STD_ADD(usize, *closure->streak_counter, 1, 887, 13);
	if (*closure->streak_counter <= 50)
		return STD_ST_CMP((const STD_ST_T *) a, (const STD_ST_T *) b);
	if (*closure->streak_counter == 50 * 2)
		*closure->streak_counter = 0;
	return STD_ORDERING_GREATER;
}

/* The instance's violate_ord_retain_orig_set closure_11's captures: the test data and the comparison, by reference. */
typedef struct STD_ST_NAME (violate_ord_retain_orig_set_closure_11_t)
{
	StdVec	   *test_data;

	STD_ST_NAME(dyn_cmp_t) * comp_func;
} STD_ST_NAME(violate_ord_retain_orig_set_closure_11_t);

/* The instance's violate_ord_retain_orig_set closure_11, catch_unwind's body: the test data's sort_by with the comparison. */
static void
STD_ST_NAME(violate_ord_retain_orig_set_closure_11) (void *captures)
{
	STD_ST_NAME(violate_ord_retain_orig_set_closure_11_t) * closure = captures;

	STD_ST_CAT(alloctests_sort_tests_stable_SortImpl_sort_by_, STD_ST_TS) (closure->test_data->ptr,
																		   closure->test_data->len,
																		   closure->comp_func->call_mut,
																		   closure->comp_func->captures, NULL);
}

/* The instance's violate_ord_retain_orig_set frame is its state across its pad: type_from_fn, the random orderings, the comparisons, and the test data, while live. */
typedef struct STD_ST_NAME (violate_ord_retain_orig_set_frame_0_t)
{
	size_t		len;
	void		(*type_into_fn) (void *captures, int32 val, STD_ST_T * result);
	void	   *type_into_fn_captures;
	int32		(*type_from_fn) (void *captures, const STD_ST_T * val);
	void	   *type_from_fn_captures;
	void		(*type_from_fn_drop) (void *);
	bool		type_from_fn_live;
	void		(*pattern_fn) (size_t len, StdVec *result);
	StdVec		random_orderings;
	bool		random_orderings_live;
	StdVec		invalid_ord_comp_functions;
	bool		invalid_ord_comp_functions_live;
	StdVec		pattern;
	StdVec		test_data;
	bool		test_data_live;
} STD_ST_NAME(violate_ord_retain_orig_set_frame_0_t);

/* The instance's Vec<Box<dyn FnMut>> drop glue: each box's, then the buffer. */
static void
STD_ST_NAME(dyn_cmp_vec_drop) (StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		STD_ST_NAME(dyn_cmp_drop) (&STD_VEC_AT(self, STD_ST_NAME(dyn_cmp_t), k));
	std_vec_free_Checked(self, sizeof(STD_ST_NAME(dyn_cmp_t)), _Alignof(STD_ST_NAME(dyn_cmp_t)));
}

/* The instance's violate_ord_retain_orig_set body: comparisons that break Ord's rules leave the elements' set. */
static void
STD_ST_NAME(violate_ord_retain_orig_set_body_0) (void *frame_)
{
	STD_ST_NAME(violate_ord_retain_orig_set_frame_0_t) * frame = frame_;
	size_t		random_orderings_len = 10000;

	STD_ST_NAME(violate_ord_retain_orig_set_closure_0_t) get_random_0_1_or_2;
	size_t		random_idx_a = 0;
	size_t		random_idx_b = 0;
	size_t		random_idx_c = 0;
	int32		last_element_a = -1;
	int32		last_element_b = -1;
	size_t		rand_counter_b = 0;
	size_t		rand_counter_c = 0;
	size_t		streak_counter_a = 0;
	size_t		streak_counter_b = 0;

	alloctests_sort_patterns_random_uniform_RangeI32(random_orderings_len, 0, 2, &frame->random_orderings);
	frame->random_orderings_live = true;
	get_random_0_1_or_2.random_orderings = &frame->random_orderings;
	{
		STD_ST_NAME(violate_ord_retain_orig_set_closure_1_t) c1 =
		{
			&get_random_0_1_or_2, &random_idx_a
		};
		STD_ST_NAME(violate_ord_retain_orig_set_closure_6_t) c6 =
		{
			&last_element_a, &last_element_b,
				frame->type_from_fn, frame->type_from_fn_captures
		};
		STD_ST_NAME(violate_ord_retain_orig_set_closure_7_t) c7 =
		{
			&rand_counter_b, &get_random_0_1_or_2,
				&random_idx_b
		};
		STD_ST_NAME(violate_ord_retain_orig_set_closure_7_t) c8 =
		{
			&rand_counter_c, &get_random_0_1_or_2,
				&random_idx_c
		};
		STD_ST_NAME(violate_ord_retain_orig_set_closure_9_t) c9 =
		{
			&streak_counter_a
		};
		STD_ST_NAME(violate_ord_retain_orig_set_closure_9_t) c10 =
		{
			&streak_counter_b
		};
		STD_ST_NAME(dyn_cmp_t) items[10];
		STD_ST_NAME(dyn_cmp_t) * boxed;

		items[0] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_1), &c1, sizeof(c1),
											 _Alignof(STD_ST_NAME(violate_ord_retain_orig_set_closure_1_t)));
		items[1] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_2), NULL, 0, 1);
		items[2] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_3), NULL, 0, 1);
		items[3] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_4), NULL, 0, 1);
		items[4] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_5), NULL, 0, 1);
		items[5] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_6), &c6, sizeof(c6),
											 _Alignof(STD_ST_NAME(violate_ord_retain_orig_set_closure_6_t)));
		items[6] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_7), &c7, sizeof(c7),
											 _Alignof(STD_ST_NAME(violate_ord_retain_orig_set_closure_7_t)));
		items[7] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_8), &c8, sizeof(c8),
											 _Alignof(STD_ST_NAME(violate_ord_retain_orig_set_closure_7_t)));
		items[8] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_9), &c9, sizeof(c9),
											 _Alignof(STD_ST_NAME(violate_ord_retain_orig_set_closure_9_t)));
		items[9] = STD_ST_NAME(dyn_cmp_new) (STD_ST_NAME(violate_ord_retain_orig_set_closure_10), &c10, sizeof(c10),
											 _Alignof(STD_ST_NAME(violate_ord_retain_orig_set_closure_9_t)));
		boxed = std_box_new_uninit_layout_Checked(sizeof(items), _Alignof(STD_ST_NAME(dyn_cmp_t)));
		memcpy(boxed, items, sizeof(items));
		std_box_slice_into_vec_Checked(&frame->invalid_ord_comp_functions, boxed, lengthof(items),
									   sizeof(STD_ST_NAME(dyn_cmp_t)));
		frame->invalid_ord_comp_functions_live = true;
	}
	for (size_t k = 0, k_len = frame->invalid_ord_comp_functions.len; k < k_len; k++)
	{
		STD_ST_NAME(dyn_cmp_t) * comp_func = &STD_VEC_AT(&frame->invalid_ord_comp_functions, STD_ST_NAME(dyn_cmp_t), k);
		int64		sum_before;
		int64		sum_after;
		bool		returned;
		StdCaught	caught;

		frame->pattern_fn(frame->len, &frame->pattern);
		STD_ST_NAME(collect_mapped) (&frame->pattern, frame->type_into_fn, frame->type_into_fn_captures, NULL,
									 &frame->test_data);
		frame->test_data_live = true;
		sum_before = STD_ST_NAME(sum_from) (&frame->test_data, frame->type_from_fn, frame->type_from_fn_captures);
		{
			STD_ST_NAME(violate_ord_retain_orig_set_closure_11_t) closure =
			{
				&frame->test_data, comp_func
			};

			returned = std_catch_unwind(STD_ST_NAME(violate_ord_retain_orig_set_closure_11), &closure, &caught);
		}
		if (!returned)
			std_Caught_drop_Checked(&caught);
		sum_after = STD_ST_NAME(sum_from) (&frame->test_data, frame->type_from_fn, frame->type_from_fn_captures);
		STD_ASSERT_EQ(912, 9, sum_before == sum_after, &sum_before, std_i64_Debug_fmt, &sum_after,
					  std_i64_Debug_fmt);
		frame->test_data_live = false;
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_data);
	}
	frame->invalid_ord_comp_functions_live = false;
	STD_ST_NAME(dyn_cmp_vec_drop) (&frame->invalid_ord_comp_functions);
	frame->random_orderings_live = false;
	std_vec_free_Checked(&frame->random_orderings, sizeof(int32), _Alignof(int32));
	frame->type_from_fn_live = false;
	if (frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
}

/* The instance's violate_ord_retain_orig_set pad: what its body holds, while live. */
static void
STD_ST_NAME(violate_ord_retain_orig_set_pad_0) (void *frame_)
{
	STD_ST_NAME(violate_ord_retain_orig_set_frame_0_t) * frame = frame_;

	if (frame->test_data_live)
		STD_ST_CAT(alloctests_sort_tests_Vec, STD_ST_CAT(STD_ST_TS, _drop)) (&frame->test_data);
	if (frame->invalid_ord_comp_functions_live)
		STD_ST_NAME(dyn_cmp_vec_drop) (&frame->invalid_ord_comp_functions);
	if (frame->random_orderings_live)
		std_vec_free_Checked(&frame->random_orderings, sizeof(int32), _Alignof(int32));
	if (frame->type_from_fn_live && frame->type_from_fn_drop != NULL)
		frame->type_from_fn_drop(frame->type_from_fn_captures);
}

/* The instance's violate_ord_retain_orig_set checks that comparisons breaking Ord leave the elements' set, taking type_from_fn. */
static void
STD_ST_NAME(violate_ord_retain_orig_set) (size_t len,
										  void (*type_into_fn) (void *captures, int32 val, STD_ST_T * result),
										  void *type_into_fn_captures,
										  int32 (*type_from_fn) (void *captures, const STD_ST_T * val),
										  void *type_from_fn_captures, void (*type_from_fn_drop) (void *),
										  void (*pattern_fn) (size_t len, StdVec *result))
{
	STD_ST_NAME(violate_ord_retain_orig_set_frame_0_t) frame =
	{
		.len = len, .type_into_fn = type_into_fn,
			.type_into_fn_captures = type_into_fn_captures, .type_from_fn = type_from_fn,
			.type_from_fn_captures = type_from_fn_captures, .type_from_fn_drop = type_from_fn_drop,
			.type_from_fn_live = true, .pattern_fn = pattern_fn
	};

	std_landing(&frame, STD_ST_NAME(violate_ord_retain_orig_set_body_0), STD_ST_NAME(violate_ord_retain_orig_set_pad_0));
}

#endif							/* STD_ST_PAIR_TS */

#endif							/* STD_ST_UNIT */

#undef STD_ST_CAT_
#undef STD_ST_CAT
#undef STD_ST_NAME
#undef STD_ST_SORT_NAME
#undef STD_ST_ELEM_SIZE
#undef STD_ST_TS
#undef STD_ST_T
#undef STD_ST_UNIT
#undef STD_ST_LT
#undef STD_ST_CLONE
#undef STD_ST_DROP
#undef STD_ST_DEBUG
#undef STD_ST_IN_PLACE
#undef STD_ST_CMP
#undef STD_ST_EQ
#undef STD_ST_PAIR_TS
#undef STD_ST_PAIR_T
