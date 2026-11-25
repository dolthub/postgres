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
 * Where nodes end (trees' chunker.rs). A node ends after an entry when the
 * hash of the entry's key falls below a threshold that rises with the node's
 * size, so boundaries depend only on the keys and their sizes. The threshold
 * follows a Weibull distribution of node sizes (shape 4, scale the target
 * size), computed with integers only so every platform draws the same
 * boundaries.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/error.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#ifdef DOLTGRES_TEST_TREES
#include "doltgres/std/test.h"
#endif
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/node.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "chunker.rs"

/* TREES_CHUNKER_LEVEL_SEEDS are LEVEL_SEEDS, the seeds of the key hash at each level, fixed random numbers. */
static const uint64 TREES_CHUNKER_LEVEL_SEEDS[16] = {
	UINT64CONST(0xcf75ee5039503236),
	UINT64CONST(0x6c9bebb1ab8562ab),
	UINT64CONST(0xcfb35c4509fb9e0d),
	UINT64CONST(0x483011df819ef277),
	UINT64CONST(0x8efc41986acddaf2),
	UINT64CONST(0x5a2320ec707e41bb),
	UINT64CONST(0x6dbc3f07f47fcf25),
	UINT64CONST(0xde7b8b197bd9cb54),
	UINT64CONST(0x592d5237aff30929),
	UINT64CONST(0x455caa3722dd4da6),
	UINT64CONST(0x48586e353695eb44),
	UINT64CONST(0xf4b76b46840aa3b5),
	UINT64CONST(0x302d3812c5427116),
	UINT64CONST(0x62d665b31e99e23b),
	UINT64CONST(0x34604aaaa477099b),
	UINT64CONST(0xcdc9d8256fb4cd53),
};

/*
 * trees_chunker_mean_node_size returns the mean size of a node of the
 * target size, which Gamma(5/4) of the target is for the Weibull
 * distribution of shape 4.
 */
uint64
trees_chunker_mean_node_size(uint32 target)
{
	return STD_DIV_FOLDED(u64, STD_MUL(u64, (uint64) target, 9064, 52, 5), 10000);
}

/*
 * trees_chunker_leaf_entry_size returns the size a leaf entry counts for
 * with its key whole, from which a splitter takes off the prefix the key
 * shares with the key before it.
 */
uint64
trees_chunker_leaf_entry_size(const uint8 *key, size_t key_len, const uint8 *value, size_t value_len)
{
	return (uint64) STD_ADD(usize, STD_ADD(usize, key_len, value_len, 58, 6), 4, 58, 5);
}

/*
 * trees_chunker_internal_entry_size returns the size an internal entry
 * counts for, which leaves out the subtree count so that a change below
 * never moves a boundary by changing a count's length.
 */
uint64
trees_chunker_internal_entry_size(const uint8 *key, size_t key_len)
{
	return (uint64) STD_ADD(usize, key_len, 40, 64, 5);
}

/* trees_chunker_Splitter_Default_default is Splitter's derived Default: every number zero and no last key. */
static TreesChunkerSplitter
trees_chunker_Splitter_Default_default(void)
{
	TreesChunkerSplitter result = {0};

	std_vec_new(&result.last, 1, 1);
	return result;
}

/*
 * trees_chunker_Splitter_grow counts an entry with the key and size into the
 * node and returns the node's size before it: sizes saturate, and at the
 * leaves after the node's first entry the entry counts for its size less the
 * prefix its key shares with the node's last key, which becomes its key.
 */
static uint64
trees_chunker_Splitter_grow(TreesChunkerSplitter *self, const uint8 *key, size_t key_len, uint64 size)
{
	uint64		before = self->size;

	self->size = std_saturating_add_u64(self->size, size);
	self->whole = std_saturating_add_u64(self->whole, size);
	if (self->level == 0)
	{
		if (self->count > 0)
		{
			uint64		shared = (uint64) trees_node_shared_prefix(self->last.ptr, self->last.len, key, key_len);

			self->size = std_saturating_sub_u64(self->size, shared);
		}
		std_vec_clear(&self->last, 1, NULL);
		std_vec_extend_from_slice_Checked(&self->last, key, key_len, 1, 1);
	}
	self->count = std_saturating_add_u32(self->count, 1);
	return before;
}

/* trees_chunker_Splitter_reset starts the next node, leaving the splitter as a new one of its level and target but for its last key. */
static void
trees_chunker_Splitter_reset(TreesChunkerSplitter *self)
{
	self->size = 0;
	self->whole = 0;
	self->count = 0;
}

/* trees_chunker_Splitter_new returns a splitter at the start of a node of the level. */
TreesChunkerSplitter
trees_chunker_Splitter_new(uint8 level, uint32 target)
{
	TreesChunkerSplitter result;

	if (!(level <= TREES_CHUNKER_MAX_LEVEL))
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("trees have at most "), STD_FMT_ARG(0),
		STD_FMT_LIT(" levels")};
		uint8		levels = (uint8) (TREES_CHUNKER_MAX_LEVEL + 1);
		StdFmtArg	args[] = {{&levels, std_u8_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 185, 9, &arguments);
	}
	result = trees_chunker_Splitter_Default_default();
	result.level = level;
	result.target = (uint64) target;
	return result;
}

/*
 * trees_chunker_Splitter_ends reports whether the node ends after an entry
 * with the key that grew it from before bytes: never after an internal
 * node's first entry, always at four targets of whole size, never below an
 * eighth of the target, and otherwise when the key's hash at the level, as
 * a u32, times t^4 falls below (size^4 - before^4) << 32.
 */
static bool
trees_chunker_Splitter_ends(const TreesChunkerSplitter *self, const uint8 *key, size_t key_len, uint64 before)
{
	uint32		hash;
	StdU128		t;
	StdU128		a;
	StdU128		b;
	StdU128		t4;
	StdU128		left;
	StdU128		a4;
	StdU128		b4;

	if (self->level > 0 && self->count == 1)
		return false;
	if (self->whole >= STD_MUL(u64, self->target, 4, 210, 26))
		return true;
	if (self->size < STD_DIV_FOLDED(u64, self->target, 8))
		return false;
	std_index_check((size_t) self->level, 16, RUST_FILE, 216, 62);
	hash = (uint32) XXH3_64bits_withSeed(key, key_len, TREES_CHUNKER_LEVEL_SEEDS[self->level]);
	t = std_u128_from_u64(self->target);
	a = std_u128_from_u64(self->size);
	b = std_u128_from_u64(before);
	t4 = std_mul_u128(std_mul_u128(std_mul_u128(t, t, RUST_FILE, 219, 27), t, RUST_FILE, 219, 27), t, RUST_FILE, 219,
					  26);
	left = std_mul_u128(std_u128_from_u64((uint64) hash), t4, RUST_FILE, 219, 9);
	a4 = std_mul_u128(std_mul_u128(std_mul_u128(a, a, RUST_FILE, 219, 45), a, RUST_FILE, 219, 45), a, RUST_FILE, 219,
					  45);
	b4 = std_mul_u128(std_mul_u128(std_mul_u128(b, b, RUST_FILE, 219, 61), b, RUST_FILE, 219, 61), b, RUST_FILE, 219,
					  61);
	return std_u128_lt(left, std_shl_folded_u128(std_sub_u128(a4, b4, RUST_FILE, 219, 44), 32));
}

/*
 * trees_chunker_Splitter_add counts an entry with the key and size and
 * reports whether the node ends after it, which also starts the next node.
 * At the leaves, the entry counts for its size less the prefix its key
 * shares with the node's last key.
 */
bool
trees_chunker_Splitter_add(TreesChunkerSplitter *self, const uint8 *key, size_t key_len, uint64 size)
{
	uint64		before;
	bool		ends;

	CHUNKS_CONTRACT(192, 9,
					self->level > 0 || self->count == 0 ||
					size >= (uint64) trees_node_shared_prefix(self->last.ptr, self->last.len, key, key_len),
					"an entry's size leaves out the key it shares a prefix of");
	before = trees_chunker_Splitter_grow(self, key, key_len, size);
	ends = trees_chunker_Splitter_ends(self, key, key_len, before);
	if (ends)
		trees_chunker_Splitter_reset(self);
	return ends;
}

/* trees_chunker_Splitter_is_empty reports whether the current node has no entries yet. */
bool
trees_chunker_Splitter_is_empty(const TreesChunkerSplitter *self)
{
	return self->count == 0;
}

/* trees_chunker_Splitter_drop is Splitter's drop glue: its last key's buffer. */
void
trees_chunker_Splitter_drop(TreesChunkerSplitter *self)
{
	std_vec_free_Checked(&self->last, 1, 1);
}

#ifdef DOLTGRES_TEST_TREES

/* test_trees_chunker_tests_node_sizes_follow_the_target_frame_0_t is the test's state across its pad: each target's splitter and sizes, while live. */
typedef struct test_trees_chunker_tests_node_sizes_follow_the_target_frame_0_t
{
	TreesChunkerSplitter splitter;
	bool		splitter_live;
	StdVec		sizes;
	bool		sizes_live;
} test_trees_chunker_tests_node_sizes_follow_the_target_frame_0_t;

/* test_trees_chunker_tests_node_sizes_follow_the_target_closure_0 is the test's |&s| s >= target / 8 && s <= target * 4 + 200. */
static bool
test_trees_chunker_tests_node_sizes_follow_the_target_closure_0(uint32 target, uint64 s)
{
	return s >= STD_DIV_FOLDED(u64, (uint64) target, 8) &&
		s <= STD_ADD(u64, STD_MUL(u64, (uint64) target, 4, 248, 74), 200, 248, 74);
}

/*
 * test_trees_chunker_tests_node_sizes_follow_the_target_body_0 is the test:
 * at each target, two million eight-byte keys of 200 bytes each, whose nodes'
 * mean size is 85% to 97% of the target and whose every node lies between an
 * eighth of the target and four targets and an entry.
 */
static void
test_trees_chunker_tests_node_sizes_follow_the_target_body_0(void *frame_)
{
	test_trees_chunker_tests_node_sizes_follow_the_target_frame_0_t *frame = frame_;
	static const uint32 targets[3] = {4096, 8192, 16384};

	for (size_t start = 0, n; std_array_IntoIter_next(&start, 3, 3, &n);)
	{
		uint32		target = targets[n];
		uint64		nodes = 0;
		uint64		size = 0;
		uint64		sum = 0;
		float8		mean;
		float8		ratio;
		bool		all = true;

		frame->splitter = trees_chunker_Splitter_new(0, target);
		frame->splitter_live = true;
		std_vec_new(&frame->sizes, sizeof(uint64), _Alignof(uint64));
		frame->sizes_live = true;
		for (uint64 i = 0, i_end = UINT64CONST(2000000); i < i_end; i = std_forward_unchecked_u64_Checked(i, 1))
		{
			uint8		key[8];

			std_u64_to_be_bytes(i, key);
			size = STD_ADD(u64, size, 200, 239, 17);
			if (trees_chunker_Splitter_add(&frame->splitter, key, 8, 200))
			{
				nodes = STD_ADD(u64, nodes, 1, 241, 21);
				std_vec_push_Checked(&frame->sizes, &size, sizeof(uint64), _Alignof(uint64));
				size = 0;
			}
		}
		for (size_t k = 0, k_len = frame->sizes.len; k < k_len; k++)
			sum = std_add_u64(sum, STD_VEC_AT(&frame->sizes, uint64, k), STD_ACCUM_FILE, 206, 1);
		mean = (float8) sum / (float8) nodes;
		ratio = mean / (float8) target;
		if (!(0.85 <= ratio && ratio < 0.97))
		{
			static const StdFmtPiece pieces[] = {STD_FMT_LIT("target "), STD_FMT_ARG(0), STD_FMT_LIT(": mean "),
			STD_FMT_ARG(1)};
			StdFmtArg	args[] = {{&target, std_u32_Display_fmt}, {&mean, std_f64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 247, 13, &arguments);
		}
		for (size_t k = 0, k_len = frame->sizes.len; k < k_len && all; k++)
			all = test_trees_chunker_tests_node_sizes_follow_the_target_closure_0(target,
																				  STD_VEC_AT(&frame->sizes, uint64, k));
		STD_ASSERT(248, 13, all, "sizes.iter().all(|&s| s >= target as u64 / 8 && s <= target as u64 * 4 + 200)");
		frame->sizes_live = false;
		std_vec_free_Checked(&frame->sizes, sizeof(uint64), _Alignof(uint64));
		frame->splitter_live = false;
		trees_chunker_Splitter_drop(&frame->splitter);
	}
}

/* test_trees_chunker_tests_node_sizes_follow_the_target_pad_0 is the test's pad: the sizes' and the splitter's drops while live. */
static void
test_trees_chunker_tests_node_sizes_follow_the_target_pad_0(void *frame_)
{
	test_trees_chunker_tests_node_sizes_follow_the_target_frame_0_t *frame = frame_;

	if (frame->sizes_live)
		std_vec_free_Checked(&frame->sizes, sizeof(uint64), _Alignof(uint64));
	if (frame->splitter_live)
		trees_chunker_Splitter_drop(&frame->splitter);
}

/* test_trees_chunker_tests_node_sizes_follow_the_target is the test node_sizes_follow_the_target. */
void
test_trees_chunker_tests_node_sizes_follow_the_target(void)
{
	test_trees_chunker_tests_node_sizes_follow_the_target_frame_0_t frame = {0};

	std_landing(&frame, test_trees_chunker_tests_node_sizes_follow_the_target_body_0,
				test_trees_chunker_tests_node_sizes_follow_the_target_pad_0);
}

/* test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_frame_0_t is the test's state across its pad: its splitter, while live. */
typedef struct test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_frame_0_t
{
	TreesChunkerSplitter splitter;
	bool		splitter_live;
} test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_frame_0_t;

/* test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_body_0 is the test: an internal node's first entry never ends it, whatever its size. */
static void
test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_body_0(void *frame_)
{
	test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_frame_0_t *frame = frame_;

	frame->splitter = trees_chunker_Splitter_new(1, 64);
	frame->splitter_live = true;
	STD_ASSERT(255, 9, !trees_chunker_Splitter_add(&frame->splitter, (const uint8 *) "k", 1, 10000),
			   "!splitter.add(b\"k\", 10_000)");
	STD_ASSERT(256, 9, trees_chunker_Splitter_add(&frame->splitter, (const uint8 *) "l", 1, 10000),
			   "splitter.add(b\"l\", 10_000)");
	frame->splitter_live = false;
	trees_chunker_Splitter_drop(&frame->splitter);
}

/* test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_pad_0 is the test's pad: the splitter's drop while live. */
static void
test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_pad_0(void *frame_)
{
	test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_frame_0_t *frame = frame_;

	if (frame->splitter_live)
		trees_chunker_Splitter_drop(&frame->splitter);
}

/* test_trees_chunker_tests_internal_nodes_never_end_after_one_entry is the test internal_nodes_never_end_after_one_entry. */
void
test_trees_chunker_tests_internal_nodes_never_end_after_one_entry(void)
{
	test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_frame_0_t frame = {0};

	std_landing(&frame, test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_body_0,
				test_trees_chunker_tests_internal_nodes_never_end_after_one_entry_pad_0);
}

#endif							/* DOLTGRES_TEST_TREES */
