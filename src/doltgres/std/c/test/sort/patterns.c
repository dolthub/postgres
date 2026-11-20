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
 * The C port of core's sort tests' patterns.rs (library/alloctests/tests/sort,
 * Rust 1.99.1): the input patterns the tests sort, random ones from an
 * XorShiftRng seeded once per process from OVERRIDE_SEED or the time in tens
 * of seconds, and their partial sorts through the unstable sort at i32.
 */
#include "postgres.h"

#include "doltgres/std/cmp.h"
#include "doltgres/std/env.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/test_flt2dec.h"
#include "doltgres/std/test_sort.h"
#include "doltgres/std/time.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE STD_RUST_LIBRARY STD_SEP "alloctests" STD_SEP "tests" STD_SEP "sort" STD_SEP "patterns.rs"

/*
 * std_i32_PartialOrd_lt_FnMut_call_mut is the fn item i32's PartialOrd::lt's
 * FnMut shim, as the sort passes it for is_less.
 */
static bool
std_i32_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_i32_PartialOrd_lt(a, b);
}

/* The unstable sort at i32, which the patterns' partial sorts use. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS I32
#define STD_SORT_T int32
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT std_i32_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/sort_template.h"

/* ALLOCTESTS_SORT_PATTERNS_SEED_VALUE is SEED_VALUE, the process's one seed. */
static StdOnceLock ALLOCTESTS_SORT_PATTERNS_SEED_VALUE = STD_ONCE_LOCK_NEW;

/* ALLOCTESTS_SORT_PATTERNS_SEED is SEED_VALUE's value, which its OnceLock guards. */
static uint64 ALLOCTESTS_SORT_PATTERNS_SEED;

static void alloctests_sort_patterns_random_vec(size_t len, StdVec *result);

/* alloctests_sort_patterns_random is random: random_vec's random i32s. */
void
alloctests_sort_patterns_random(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_vec(len, result);
}

/*
 * alloctests_sort_patterns_random_uniform_collect is random_uniform's
 * rng.sample_iter(dist).take(len).collect(): SpecFromIterNested's first
 * draw, then extend_desugared's, Take's count wrapping down, its size_hint
 * the count left.
 */
static void
alloctests_sort_patterns_random_uniform_collect(uint32 *rng, const CoretestsUniformInt *dist, size_t len,
												StdVec *result)
{
	size_t		n = len;
	int32		element;

	if (n == 0)
	{
		std_vec_new(result, sizeof(int32), _Alignof(int32));
		return;
	}
	n = std_wrapping_sub_usize(n, 1);
	element = (int32) (uint32) coretests_rand_UniformInt_sample(dist, rng, 32, 32);
	std_vec_collect_first(result, &element, n, sizeof(int32), _Alignof(int32));
	while (n != 0)
	{
		n = std_wrapping_sub_usize(n, 1);
		element = (int32) (uint32) coretests_rand_UniformInt_sample(dist, rng, 32, 32);
		std_vec_extend_one(result, &element, n, sizeof(int32), _Alignof(int32));
	}
}

/*
 * alloctests_sort_patterns_random_uniform_RangeI32 is random_uniform at a
 * Range<i32>: Uniform::try_from's new over the range, unwrapped, and len
 * draws from it.
 */
void
alloctests_sort_patterns_random_uniform_RangeI32(size_t len, int32 start, int32 end, StdVec *result)
{
	uint32		rng[4];
	CoretestsUniformInt dist;

	coretests_XorShiftRng_seed_from_u64(alloctests_sort_patterns_get_or_init_rand_seed(), rng);
	dist = coretests_rand_UniformInt_new((uint32) start, (uint32) end, true, 32, 32, RUST_FILE, 33, 41);
	alloctests_sort_patterns_random_uniform_collect(rng, &dist, len, result);
}

/*
 * alloctests_sort_patterns_random_uniform_RangeInclusiveI32 is
 * random_uniform at a RangeInclusive<i32>: Uniform::try_from's
 * new_inclusive over the range, unwrapped, and len draws from it.
 */
void
alloctests_sort_patterns_random_uniform_RangeInclusiveI32(size_t len, int32 start, int32 end, StdVec *result)
{
	uint32		rng[4];
	CoretestsUniformInt dist;

	coretests_XorShiftRng_seed_from_u64(alloctests_sort_patterns_get_or_init_rand_seed(), rng);
	dist = coretests_rand_UniformInt_new_inclusive((uint32) start, (uint32) end, true, 32, 32, RUST_FILE, 33, 41);
	alloctests_sort_patterns_random_uniform_collect(rng, &dist, len, result);
}

/* alloctests_sort_patterns_random_zipf_closure_0 is random_zipf's |val| val as i32. */
static int32
alloctests_sort_patterns_random_zipf_closure_0(size_t val)
{
	return (int32) val;
}

/*
 * alloctests_sort_patterns_random_zipf is random_zipf: ZipfDistribution::new
 * over the length, unwrapped, and len draws from it as i32s, collected as
 * random_uniform's are.
 */
void
alloctests_sort_patterns_random_zipf(size_t len, float8 exponent, StdVec *result)
{
	uint32		rng[4];
	AlloctestsSortZipfDistribution dist;
	size_t		n = len;
	int32		element;

	coretests_XorShiftRng_seed_from_u64(alloctests_sort_patterns_get_or_init_rand_seed(), rng);
	if (!alloctests_sort_zipf_ZipfDistribution_new(len, exponent, &dist))
	{
		static const char unit = 0;

		STD_RESULT_EXPECT_FAILED(42, 53, "called `Result::unwrap()` on an `Err` value", &unit,
								 std_unit_Debug_fmt);
	}
	if (n == 0)
	{
		std_vec_new(result, sizeof(int32), _Alignof(int32));
		return;
	}
	n = std_wrapping_sub_usize(n, 1);
	element = alloctests_sort_patterns_random_zipf_closure_0(alloctests_sort_zipf_ZipfDistribution_next(&dist, rng));
	std_vec_collect_first(result, &element, n, sizeof(int32), _Alignof(int32));
	while (n != 0)
	{
		n = std_wrapping_sub_usize(n, 1);
		element = alloctests_sort_patterns_random_zipf_closure_0(alloctests_sort_zipf_ZipfDistribution_next(&dist,
																											rng));
		std_vec_extend_one(result, &element, n, sizeof(int32), _Alignof(int32));
	}
}

/*
 * alloctests_sort_patterns_random_sorted is random_sorted: random_vec with
 * its first sorted_percent percent, rounded, sorted.
 */
void
alloctests_sort_patterns_random_sorted(size_t len, float8 sorted_percent, StdVec *result)
{
	size_t		sorted_len;

	alloctests_sort_patterns_random_vec(len, result);
	sorted_len = std_as_f64_usize(std_f64_round((float8) len * (sorted_percent / 100.0)));
	std_range_check(0, sorted_len, result->len, RUST_FILE, 61, 6);
	std_slice_sort_unstable_I32(result->ptr, sorted_len);
}

/*
 * alloctests_sort_patterns_all_equal is all_equal: vec![66; len], from_elem's
 * with_capacity and extend_with, its last element moved in.
 */
void
alloctests_sort_patterns_all_equal(size_t len, StdVec *result)
{
	int32		elem = 66;

	std_vec_with_capacity_Checked(result, len, sizeof(int32), _Alignof(int32));
	std_vec_reserve_Checked(result, len, sizeof(int32), _Alignof(int32));
	for (size_t i = 0; i < len; i++)
	{
		STD_VEC_AT(result, int32, result->len) = elem;
		result->len = std_wrapping_add_usize(result->len, 1);
	}
}

/*
 * alloctests_sort_patterns_ascending is ascending: (0..len as i32)'s
 * TrustedLen collect, with_capacity and extend_trusted of its exact count,
 * each value written past the length.
 */
void
alloctests_sort_patterns_ascending(size_t len, StdVec *result)
{
	int32		start = 0;
	int32		end = (int32) len;
	StdSizeHint hint = {0, true, 0};

	if (start <= end)
	{
		hint.lower = (size_t) std_wrapping_sub_i64((int64) end, (int64) start);
		hint.upper = hint.lower;
	}
	std_vec_with_capacity_trusted(result, hint, sizeof(int32), _Alignof(int32));
	std_vec_extend_trusted_reserve(result, hint, sizeof(int32), _Alignof(int32));
	while (start < end)
	{
		int32		old = start;

		start = std_forward_unchecked_i32_Checked(old, 1);
		STD_VEC_AT(result, int32, result->len) = old;
		result->len = std_wrapping_add_usize(result->len, 1);
	}
}

/*
 * alloctests_sort_patterns_descending is descending: (0..len as i32).rev()'s
 * TrustedLen collect, as ascending's, its values from next_back.
 */
void
alloctests_sort_patterns_descending(size_t len, StdVec *result)
{
	int32		start = 0;
	int32		end = (int32) len;
	StdSizeHint hint = {0, true, 0};

	if (start <= end)
	{
		hint.lower = (size_t) std_wrapping_sub_i64((int64) end, (int64) start);
		hint.upper = hint.lower;
	}
	std_vec_with_capacity_trusted(result, hint, sizeof(int32), _Alignof(int32));
	std_vec_extend_trusted_reserve(result, hint, sizeof(int32), _Alignof(int32));
	while (start < end)
	{
		end = std_backward_unchecked_i32_Checked(end, 1);
		STD_VEC_AT(result, int32, result->len) = end;
		result->len = std_wrapping_add_usize(result->len, 1);
	}
}

/* alloctests_sort_patterns_saw_mixed_closure_0 is saw_mixed's |&e| Reverse(e): the key, Reverse<i32>'s bits. */
static int32
alloctests_sort_patterns_saw_mixed_closure_0(const int32 *e)
{
	return *e;
}

/* alloctests_sort_patterns_saw_mixed_range_closure_0 is saw_mixed_range's |&e| Reverse(e). */
static int32
alloctests_sort_patterns_saw_mixed_range_closure_0(const int32 *e)
{
	return *e;
}

/* alloctests_sort_patterns_pipe_organ_closure_0 is pipe_organ's |&e| Reverse(e). */
static int32
alloctests_sort_patterns_pipe_organ_closure_0(const int32 *e)
{
	return *e;
}

/* std_Reverse_I32_PartialOrd_lt is Reverse<i32>'s PartialOrd::lt: other.0 < self.0. */
static bool
std_Reverse_I32_PartialOrd_lt(const int32 *self, const int32 *other)
{
	return *other < *self;
}

/* std_slice_sort_unstable_by_key_closure_0 is sort_unstable_by_key's |a, b| f(a).lt(&f(b)) at a Reverse<i32> key. */
static bool
std_slice_sort_unstable_by_key_closure_0(void *captures, const void *a, const void *b)
{
	int32		(*f) (const int32 *) = (int32 (*) (const int32 *)) captures;
	int32		ka = f(a);
	int32		kb = f(b);

	return std_Reverse_I32_PartialOrd_lt(&ka, &kb);
}

/* std_slice_sort_unstable_by_key_I32ReverseI32 is <[i32]>::sort_unstable_by_key at a Reverse<i32> key: the unstable sort by f's keys. */
static void
std_slice_sort_unstable_by_key_I32ReverseI32(int32 *v, size_t len, int32 (*f) (const int32 *))
{
	StdSortLess is_less = {std_slice_sort_unstable_by_key_closure_0, (void *) f};

	std_sort_sort_I32(v, len, &is_less);
}

/*
 * alloctests_sort_patterns_saw_mixed is saw_mixed: random_vec in chunks of
 * len / saw_count, each sorted ascending or descending by a random 0 or 1.
 */
void
alloctests_sort_patterns_saw_mixed(size_t len, size_t saw_count, StdVec *result)
{
	size_t		chunks_size;
	StdVec		saw_directions;
	StdChunksMut chunks;
	void	   *chunk;
	size_t		chunk_len;
	size_t		count = 0;

	if (len == 0)
	{
		std_vec_new(result, sizeof(int32), _Alignof(int32));
		return;
	}
	alloctests_sort_patterns_random_vec(len, result);
	chunks_size = STD_DIV(usize, len, std_max_usize(saw_count, 1), 98, 23);
	alloctests_sort_patterns_random_uniform_RangeInclusiveI32(STD_ADD(usize, STD_DIV(usize, len, chunks_size, 99, 42),
																	  1, 99, 41), 0, 1, &saw_directions);
	chunks = std_slice_chunks_mut(result->ptr, result->len, chunks_size, RUST_FILE, 101, 28);
	while (std_slice_ChunksMut_next(&chunks, sizeof(int32), &chunk, &chunk_len))
	{
		size_t		i = count;

		count = std_Enumerate_count_step(count);
		std_index_check(i, saw_directions.len, RUST_FILE, 102, 26);
		if (STD_VEC_AT(&saw_directions, int32, i) == 0)
			std_slice_sort_unstable_I32(chunk, chunk_len);
		else
		{
			std_index_check(i, saw_directions.len, RUST_FILE, 104, 33);
			if (STD_VEC_AT(&saw_directions, int32, i) == 1)
				std_slice_sort_unstable_by_key_I32ReverseI32(chunk, chunk_len, alloctests_sort_patterns_saw_mixed_closure_0);
			else
				STD_PANIC(107, 13, "internal error: entered unreachable code");
		}
	}
	std_vec_free_Checked(&saw_directions, sizeof(int32), _Alignof(int32));
}

/*
 * alloctests_sort_patterns_saw_mixed_range is saw_mixed_range: random_vec in
 * chunks of random sizes in the range, each sorted ascending or descending
 * by a random 0 or 1.
 */
void
alloctests_sort_patterns_saw_mixed_range(size_t len, size_t range_start, size_t range_end, StdVec *result)
{
	size_t		max_chunks;
	StdVec		saw_directions;
	StdVec		chunk_sizes;
	size_t		i = 0;
	size_t		l = 0;

	if (len == 0)
	{
		std_vec_new(result, sizeof(int32), _Alignof(int32));
		return;
	}
	alloctests_sort_patterns_random_vec(len, result);
	max_chunks = STD_DIV(usize, len, range_start, 127, 22);
	alloctests_sort_patterns_random_uniform_RangeInclusiveI32(STD_ADD(usize, max_chunks, 1, 128, 41), 0, 1,
															  &saw_directions);
	alloctests_sort_patterns_random_uniform_RangeI32(STD_ADD(usize, max_chunks, 1, 129, 38), (int32) range_start,
													 (int32) range_end, &chunk_sizes);
	while (l < len)
	{
		size_t		chunk_size;
		size_t		chunk_end;

		std_index_check(i, chunk_sizes.len, RUST_FILE, 134, 37);
		chunk_size = (size_t) (int64) STD_VEC_AT(&chunk_sizes, int32, i);
		chunk_end = std_min_usize(STD_ADD(usize, l, chunk_size, 135, 39), len);
		std_range_check(l, chunk_end, result->len, RUST_FILE, 136, 30);
		std_index_check(i, saw_directions.len, RUST_FILE, 138, 26);
		if (STD_VEC_AT(&saw_directions, int32, i) == 0)
			std_slice_sort_unstable_I32(&STD_VEC_AT(result, int32, l), std_wrapping_sub_usize(chunk_end, l));
		else
		{
			std_index_check(i, saw_directions.len, RUST_FILE, 140, 33);
			if (STD_VEC_AT(&saw_directions, int32, i) == 1)
				std_slice_sort_unstable_by_key_I32ReverseI32(&STD_VEC_AT(result, int32, l),
															 std_wrapping_sub_usize(chunk_end, l),
															 alloctests_sort_patterns_saw_mixed_range_closure_0);
			else
				STD_PANIC(143, 13, "internal error: entered unreachable code");
		}
		i = STD_ADD(usize, i, 1, 146, 9);
		l = STD_ADD(usize, l, chunk_size, 147, 9);
	}
	std_vec_free_Checked(&chunk_sizes, sizeof(int32), _Alignof(int32));
	std_vec_free_Checked(&saw_directions, sizeof(int32), _Alignof(int32));
}

/*
 * alloctests_sort_patterns_pipe_organ is pipe_organ: random_vec with its
 * first half sorted ascending and its second half descending.
 */
void
alloctests_sort_patterns_pipe_organ(size_t len, StdVec *result)
{
	size_t		half;

	alloctests_sort_patterns_random_vec(len, result);
	half = STD_DIV_FOLDED(usize, len, 2);
	std_range_check(0, half, result->len, RUST_FILE, 159, 31);
	std_slice_sort_unstable_I32(result->ptr, half);
	half = STD_DIV_FOLDED(usize, len, 2);
	std_range_check(half, len, result->len, RUST_FILE, 162, 32);
	std_slice_sort_unstable_by_key_I32ReverseI32(&STD_VEC_AT(result, int32, half), std_wrapping_sub_usize(len, half),
												 alloctests_sort_patterns_pipe_organ_closure_0);
}

/*
 * alloctests_sort_patterns_rand_root_seed is rand_root_seed outside Miri: the
 * seconds since the Unix epoch, unwrapped, in tens.
 */
static uint64
alloctests_sort_patterns_rand_root_seed(void)
{
	StdSystemTime now = std_SystemTime_now();
	StdDuration since;
	StdDuration error;

	if (!std_SystemTime_duration_since(&now, STD_UNIX_EPOCH, &since, &error))
		STD_RESULT_EXPECT_FAILED(192, 70, "called `Result::unwrap()` on an `Err` value", &error,
								 std_SystemTimeError_Debug_fmt);
	return STD_DIV_FOLDED(u64, std_Duration_as_secs(&since), 10);
}

/* alloctests_sort_patterns_get_or_init_rand_seed_closure_1_frame_0_t is the map closure's state across its pad: the String it owns, while live, and its u64. */
typedef struct alloctests_sort_patterns_get_or_init_rand_seed_closure_1_frame_0_t
{
	StdString	seed;
	bool		seed_live;
	uint64		result;
} alloctests_sort_patterns_get_or_init_rand_seed_closure_1_frame_0_t;

/* alloctests_sort_patterns_get_or_init_rand_seed_closure_1_body_0 is the map closure, |seed| u64::from_str(&seed).unwrap(), which drops seed after. */
static void
alloctests_sort_patterns_get_or_init_rand_seed_closure_1_body_0(void *frame_)
{
	alloctests_sort_patterns_get_or_init_rand_seed_closure_1_frame_0_t *frame = frame_;
	StdParseIntError parse;

	if (!std_str_parse_u64(frame->seed.ptr, frame->seed.len, &frame->result, &parse))
		STD_RESULT_EXPECT_FAILED(172, 46, "called `Result::unwrap()` on an `Err` value", &parse,
								 std_ParseIntError_Debug_fmt);
	frame->seed_live = false;
	std_vec_free_Checked(&frame->seed, 1, 1);
}

/* alloctests_sort_patterns_get_or_init_rand_seed_closure_1_pad_0 is the map closure's pad: seed, while live. */
static void
alloctests_sort_patterns_get_or_init_rand_seed_closure_1_pad_0(void *frame_)
{
	alloctests_sort_patterns_get_or_init_rand_seed_closure_1_frame_0_t *frame = frame_;

	if (frame->seed_live)
		std_vec_free_Checked(&frame->seed, 1, 1);
}

/*
 * alloctests_sort_patterns_get_or_init_rand_seed_closure_0 is
 * get_or_init_rand_seed's closure: OVERRIDE_SEED's u64, unwrapped, or
 * rand_root_seed's when it isn't set or isn't UTF-8.
 */
static void
alloctests_sort_patterns_get_or_init_rand_seed_closure_0(void *captures, void *result)
{
	alloctests_sort_patterns_get_or_init_rand_seed_closure_1_frame_0_t frame = {0};
	StdVarError error;

	if (std_env_var("OVERRIDE_SEED", &frame.seed, &error))
	{
		frame.seed_live = true;
		std_landing(&frame, alloctests_sort_patterns_get_or_init_rand_seed_closure_1_body_0,
					alloctests_sort_patterns_get_or_init_rand_seed_closure_1_pad_0);
		*(uint64 *) result = frame.result;
		return;
	}
	std_VarError_drop(&error);
	*(uint64 *) result = alloctests_sort_patterns_rand_root_seed();
}

/* alloctests_sort_patterns_get_or_init_rand_seed is get_or_init_rand_seed: SEED_VALUE, set once per process by its closure. */
uint64
alloctests_sort_patterns_get_or_init_rand_seed(void)
{
	std_once_lock_get_or_init(&ALLOCTESTS_SORT_PATTERNS_SEED_VALUE, &ALLOCTESTS_SORT_PATTERNS_SEED,
							  alloctests_sort_patterns_get_or_init_rand_seed_closure_0, NULL);
	return ALLOCTESTS_SORT_PATTERNS_SEED;
}

/*
 * alloctests_sort_patterns_random_vec is random_vec: len random i32s from a
 * seeded XorShiftRng's random_iter, collected as random_uniform's are.
 */
static void
alloctests_sort_patterns_random_vec(size_t len, StdVec *result)
{
	uint32		rng[4];
	size_t		n = len;
	int32		element;

	coretests_XorShiftRng_seed_from_u64(alloctests_sort_patterns_get_or_init_rand_seed(), rng);
	if (n == 0)
	{
		std_vec_new(result, sizeof(int32), _Alignof(int32));
		return;
	}
	n = std_wrapping_sub_usize(n, 1);
	element = rand_StandardUniform_sample_i32(rng);
	std_vec_collect_first(result, &element, n, sizeof(int32), _Alignof(int32));
	while (n != 0)
	{
		n = std_wrapping_sub_usize(n, 1);
		element = rand_StandardUniform_sample_i32(rng);
		std_vec_extend_one(result, &element, n, sizeof(int32), _Alignof(int32));
	}
}
