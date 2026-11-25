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
 * The C forms of core's slice methods the crates use (core/src/slice/mod.rs
 * at Rust 1.99.1): split_at_mut, copy_from_slice, split_at, and chunks'
 * check, each #[track_caller], so their panics name their caller's line.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* split_at as the crates' code runs it, checked in the cassert build, and as core's own code does, unchecked. */
#define STD_SPLIT_DEFINE
#define STD_SPLIT_UB_CHECKS 1
#define STD_SPLIT_SUFFIX Checked
#include "doltgres/std/slice_split_template.h"
#define STD_SPLIT_DEFINE
#define STD_SPLIT_UB_CHECKS 0
#define STD_SPLIT_SUFFIX Unchecked
#include "doltgres/std/slice_split_template.h"

#define INTRINSICS_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "intrinsics" STD_SEP "mod.rs"

/* index.rs */

/* std_index_check is the bounds check of `v[i]`, with Rust's panic at its caller. */
void
std_index_check(size_t index, size_t len, const char *file, uint32 line, uint32 col)
{
	if (index >= len)
		std_vec_panic_numbers(file, line, col,
							  "index out of bounds: the len is %zu but the index is %zu",
							  len, index);
}

/*
 * std_slice_index_fail is core's panic of a range that doesn't fit a slice, its
 * checks in core's order.
 */
pg_noreturn static void
std_slice_index_fail(size_t start, size_t end, size_t len, const char *file,
					 uint32 line, uint32 col)
{
	if (start > len)
		std_vec_panic_numbers(file, line, col,
							  "range start index %zu out of range for slice of length %zu",
							  start, len);
	if (end > len)
		std_vec_panic_numbers(file, line, col,
							  "range end index %zu out of range for slice of length %zu",
							  end, len);
	if (start > end)
		std_vec_panic_numbers(file, line, col,
							  "slice index starts at %zu but ends at %zu", start, end);
	std_vec_panic_numbers(file, line, col,
						  "range end index %zu out of range for slice of length %zu",
						  end, len);
}

/* std_range_check is the bounds check of `v[start..end]`, with core's panics at its caller. */
void
std_range_check(size_t start, size_t end, size_t len, const char *file,
				uint32 line, uint32 col)
{
	if (start > end || end > len)
		std_slice_index_fail(start, end, len, file, line, col);
}

/* std_range_from_check is the bounds check of `v[start..]`, with core's panic at its caller. */
void
std_range_from_check(size_t start, size_t len, const char *file, uint32 line, uint32 col)
{
	if (start > len)
		std_slice_index_fail(start, len, len, file, line, col);
}

/* std_range_to_check is the bounds check of `v[..end]`, with core's panic at its caller. */
void
std_range_to_check(size_t end, size_t len, const char *file, uint32 line, uint32 col)
{
	if (end > len)
		std_slice_index_fail(0, end, len, file, line, col);
}

/*
 * std_range_inclusive_check is the bounds check of `v[start..=end]` for a
 * range not yet iterated, RangeInclusive's index: an end below the length,
 * made exclusive, and a start at most it, else core's panic at its caller.
 */
void
std_range_inclusive_check(size_t start, size_t end, size_t len, const char *file, uint32 line, uint32 col)
{
	if (end < len)
	{
		end = std_wrapping_add_usize(end, 1);
		if (start <= end)
			return;
	}
	std_slice_index_fail(start, end, len, file, line, col);
}

/*
 * std_slice_range is core::slice::range of a range within ..len, its body
 * core's into_slice_range: the range's bounds as its start and end, failing
 * with slice_index_fail's panics at core's own lines, since
 * into_slice_range doesn't track its caller.
 */
void
std_slice_range(StdBound start, StdBound end, size_t len, size_t *range_start, size_t *range_end)
{
	switch (end.tag)
	{
		case STD_BOUND_INCLUDED:
			if (end.index >= len)
				std_slice_index_fail(0, end.index, len, STD_SLICE_INDEX_FILE, 1005, 52);
			*range_end = end.index + 1;
			break;
		case STD_BOUND_EXCLUDED:
			if (end.index > len)
				std_slice_index_fail(0, end.index, len, STD_SLICE_INDEX_FILE, 1009, 51);
			*range_end = end.index;
			break;
		case STD_BOUND_UNBOUNDED:
			*range_end = len;
			break;
	}
	switch (start.tag)
	{
		case STD_BOUND_EXCLUDED:
			if (start.index >= *range_end)
				std_slice_index_fail(start.index, *range_end, len, STD_SLICE_INDEX_FILE, 1016, 56);
			*range_start = start.index + 1;
			break;
		case STD_BOUND_INCLUDED:
			if (start.index > *range_end)
				std_slice_index_fail(start.index, *range_end, len, STD_SLICE_INDEX_FILE, 1020, 55);
			*range_start = start.index;
			break;
		case STD_BOUND_UNBOUNDED:
			*range_start = 0;
			break;
	}
}

/* mod.rs */

/*
 * std_slice_split_at_mut_unchecked is <[T]>::split_at_mut_unchecked: the
 * slice's two halves at mid, its precondition and from_raw_parts_mut's
 * checked at its caller.
 */
static void
std_slice_split_at_mut_unchecked(void *ptr, size_t len, size_t mid, size_t size, size_t align, void **left,
								 size_t *left_len, void **right, size_t *right_len, const char *file, uint32 line,
								 uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!(mid <= len))
		STD_PTR_VIOLATED("slice::split_at_mut_unchecked requires the index to be within the slice", file, line, col);
#endif
	std_slice_from_raw_parts_mut_check(ptr, mid, size, align, file, line, col);
	*left = ptr;
	*left_len = mid;
	std_slice_from_raw_parts_mut_check((char *) ptr + mid * size, len - mid, size, align, file, line, col);
	*right = (char *) ptr + mid * size;
	*right_len = len - mid;
}

/*
 * std_slice_split_at_mut_checked is <[T]>::split_at_mut_checked: the
 * slice's two halves at mid, or false for a mid past the length.
 */
static bool
std_slice_split_at_mut_checked(void *ptr, size_t len, size_t mid, size_t size, size_t align, void **left,
							   size_t *left_len, void **right, size_t *right_len)
{
	if (mid <= len)
	{
		std_slice_split_at_mut_unchecked(ptr, len, mid, size, align, left, left_len, right, right_len,
										 STD_SLICE_MOD_FILE, 2205, 32);
		return true;
	}
	return false;
}

/*
 * std_slice_split_at_unchecked is <[T]>::split_at_unchecked: the slice's
 * two halves at mid, its precondition and from_raw_parts's checked at its
 * caller.
 */
static void
std_slice_split_at_unchecked(const void *ptr, size_t len, size_t mid, size_t size, size_t align, const void **left,
							 size_t *left_len, const void **right, size_t *right_len, const char *file, uint32 line,
							 uint32 col)
{
	const void *after = STD_PTR_AT(ptr, mid, size);
	size_t		after_len = std_wrapping_sub_usize(len, mid);

#ifdef USE_ASSERT_CHECKING
	if (!(mid <= len))
		STD_PTR_VIOLATED("slice::split_at_unchecked requires the index to be within the slice", file, line, col);
#endif
	std_slice_from_raw_parts_check(ptr, mid, size, align, file, line, col);
	*left = ptr;
	*left_len = mid;
	std_slice_from_raw_parts_check(after, after_len, size, align, file, line, col);
	*right = after;
	*right_len = after_len;
}

/*
 * std_slice_split_at_checked is <[T]>::split_at_checked: the slice's two
 * halves at mid, or false for a mid past the length.
 */
bool
std_slice_split_at_checked(const void *ptr, size_t len, size_t mid, size_t size, size_t align, const void **left,
						   size_t *left_len, const void **right, size_t *right_len)
{
	if (mid <= len)
	{
		std_slice_split_at_unchecked(ptr, len, mid, size, align, left, left_len, right, right_len,
									 STD_SLICE_MOD_FILE, 2166, 32);
		return true;
	}
	return false;
}

/*
 * std_slice_split_last_chunk is <[T]>::split_last_chunk::<N>: the slice
 * without its last n elements and those n, or false for a slice shorter than
 * n; split_at's "mid > len" names split_last_chunk's call of it.
 */
bool
std_slice_split_last_chunk(const void *ptr, size_t len, size_t n, size_t size, size_t align, const void **init,
						   size_t *init_len, const void **last)
{
	size_t		index;
	size_t		last_len;

	if (!std_checked_sub_usize(len, n, &index))
		return false;
	if (!std_slice_split_at_checked(ptr, len, index, size, align, init, init_len, last, &last_len))
	{
		static const char text[] = "mid > len";

		std_panic(STD_SLICE_MOD_FILE, 449, 33, text, sizeof(text) - 1);
	}
	return true;
}

/* std_slice_split_u8 is <[u8]>::split: a Split over the slice by a predicate, not yet finished. */
StdSliceSplit
std_slice_split_u8(const uint8 *v, size_t len, bool (*pred) (void *captures, const uint8 *b), void *captures)
{
	StdSliceSplit split = {v, len, pred, captures, false};

	return split;
}

/*
 * std_SliceSplit_next is Split's next: the part before the next element the
 * predicate takes, the rest kept past it, or, with none, the whole of what is
 * left, once, through finish; false once it is finished.
 */
bool
std_SliceSplit_next(StdSliceSplit *self, const uint8 **piece, size_t *piece_len)
{
	size_t		idx = 0;
	bool		found = false;

	if (self->finished)
		return false;
	for (size_t i = 0, i_len = self->v_len; i < i_len; i++)
	{
		if (self->pred(self->captures, &self->v[i]))
		{
			idx = i;
			found = true;
			break;
		}
	}
	if (!found)
	{
		self->finished = true;
		*piece = self->v;
		*piece_len = self->v_len;
		return true;
	}
	std_slice_get_unchecked_range_check(0, idx, self->v_len, STD_SLICE_INDEX_FILE, 492, 32);
	std_slice_get_unchecked_range_check(idx + 1, self->v_len, self->v_len, STD_SLICE_INDEX_FILE, 533, 44);
	*piece = self->v;
	*piece_len = idx;
	self->v = self->v + idx + 1;
	self->v_len = self->v_len - (idx + 1);
	return true;
}

/*
 * std_slice_split_at_mut is <[T]>::split_at_mut: the slice's two halves at
 * mid, panicking with "mid > len" at its caller for a mid past the length.
 */
void
std_slice_split_at_mut(void *ptr, size_t len, size_t mid, size_t size, size_t align, void **left,
					   size_t *left_len, void **right, size_t *right_len, const char *file, uint32 line, uint32 col)
{
	if (!std_slice_split_at_mut_checked(ptr, len, mid, size, align, left, left_len, right, right_len))
	{
		static const char text[] = "mid > len";

		std_panic(file, line, col, text, sizeof(text) - 1);
	}
}

/*
 * std_iter_step_by_range_usize is a Range<usize>'s step_by, StepBy::new:
 * core's step != 0 assertion, then SpecRangeSetup's setup, which returns how
 * many items the steps from the range's start yield, its new end.
 */
size_t
std_iter_step_by_range_usize(size_t start, size_t end, size_t step)
{
	size_t		inner_len;

	if (!(step != 0))
	{
		static const char text[] = "assertion failed: step != 0";

		std_panic(STD_STEP_BY_FILE, 36, 9, text, sizeof(text) - 1);
	}
	inner_len = start < end ? std_wrapping_sub_usize(end, start) : 0;
	return std_div_ceil_usize(inner_len, step, STD_STEP_BY_FILE, 621, 5);
}

/*
 * std_iter_step_by_range_u64 is a Range<u64>'s step_by, StepBy::new: core's
 * step != 0 assertion, then SpecRangeSetup's setup, which returns how many
 * items the steps from the range's start yield, its new end.
 */
uint64
std_iter_step_by_range_u64(uint64 start, uint64 end, size_t step)
{
	uint64		inner_len;

	if (!(step != 0))
	{
		static const char text[] = "assertion failed: step != 0";

		std_panic(STD_STEP_BY_FILE, 36, 9, text, sizeof(text) - 1);
	}
	inner_len = start < end ? std_wrapping_sub_u64(end, start) : 0;
	return std_div_ceil_u64(inner_len, (uint64) step, STD_STEP_BY_FILE, 621, 5);
}

/*
 * std_slice_Iter_step_by is a slice Iter's step_by, StepBy::new: core's
 * step != 0 assertion, and the iterator taking its first item next.
 */
StdSliceStepBy
std_slice_Iter_step_by(const void *v, size_t len, size_t step)
{
	if (!(step != 0))
	{
		static const char text[] = "assertion failed: step != 0";

		std_panic(STD_STEP_BY_FILE, 36, 9, text, sizeof(text) - 1);
	}
	return (StdSliceStepBy)
	{
		v, len, step - 1, true
	};
}

/*
 * std_SliceStepBy_Iterator_next is StepBy's next over a slice Iter: the
 * slice Iter's nth of no item first, then of step - 1, and false once none
 * is left.
 */
bool
std_SliceStepBy_Iterator_next(StdSliceStepBy *self, size_t size, const void **result)
{
	size_t		n = self->first_take ? 0 : self->step_minus_one;

	self->first_take = false;
	if (n >= self->len)
	{
		self->v = STD_PTR_AT(self->v, self->len, size);
		self->len = 0;
		return false;
	}
	*result = STD_PTR_AT(self->v, n, size);
	self->v = STD_PTR_AT(self->v, n + 1, size);
	self->len -= n + 1;
	return true;
}

/*
 * std_slice_chunks_check is <[T]>::chunks' assertion, panicking with "chunk
 * size must be non-zero" at its caller for a chunk size of zero.
 */
void
std_slice_chunks_check(size_t chunk_size, const char *file, uint32 line, uint32 col)
{
	if (!(chunk_size != 0))
	{
		static const char text[] = "chunk size must be non-zero";

		std_panic(file, line, col, text, sizeof(text) - 1);
	}
}

/* std_slice_align_to_offsets_gcd is align_to_offsets' gcd, which core evaluates as a constant. */
static size_t
std_slice_align_to_offsets_gcd(size_t a, size_t b)
{
	return b == 0 ? a : std_slice_align_to_offsets_gcd(b, a % b);
}

/*
 * std_slice_align_to_offsets is <[T]>::align_to_offsets: how many Us the
 * fewest Ts a slice's rest holds make, and how many Ts trail them.
 */
static void
std_slice_align_to_offsets(size_t len, size_t t_size, size_t u_size, size_t *us_len, size_t *ts_len)
{
	size_t		gcd = std_slice_align_to_offsets_gcd(t_size, u_size);
	size_t		ts = STD_DIV_FOLDED(usize, u_size, gcd);
	size_t		us = STD_DIV_FOLDED(usize, t_size, gcd);

	*us_len = STD_DIV_FOLDED(usize, len, ts) * us;
	*ts_len = STD_REM_FOLDED(usize, len, ts);
}

/*
 * std_slice_align_to is <[T]>::align_to as core's own code runs it: the
 * slice's Ts before its first U-aligned address, the Us after, and the Ts
 * left over, or the whole slice before when no U-aligned address is in it.
 */
void
std_slice_align_to(const void *ptr, size_t len, size_t t_size, size_t t_align, size_t u_size, size_t u_align,
				   StdSliceAlignTo *result)
{
	size_t		offset;

	if (u_size == 0 || t_size == 0 || (offset = std_ptr_align_offset(ptr, t_size, u_align)) > len)
	{
		result->head = ptr;
		result->head_len = len;
		result->body = STD_DANGLING_FOR(u_align);
		result->body_len = 0;
		result->tail = STD_DANGLING_FOR(t_align);
		result->tail_len = 0;
	}
	else
	{
		const void *rest;
		size_t		rest_len;
		size_t		ts_len;

		std_slice_split_at_Unchecked(ptr, len, offset, t_size, t_align, &result->head, &result->head_len, &rest,
									 &rest_len, STD_SLICE_MOD_FILE, 4534, 37);
		std_slice_align_to_offsets(rest_len, t_size, u_size, &result->body_len, &ts_len);
		result->body = rest;
		result->tail = (const char *) rest + (rest_len - ts_len) * t_size;
		result->tail_len = ts_len;
	}
}

/*
 * std_slice_chunks is <[T]>::chunks as core's own code runs it: the
 * iterator over chunks of a constant size, whose assertion of a size other
 * than zero shipped core's MIR proves away.
 */
StdChunks
std_slice_chunks(const void *v, size_t v_len, size_t chunk_size)
{
	return (StdChunks)
	{
		v, v_len, chunk_size
	};
}

/* std_slice_Chunks_next is Chunks::next: the next chunk, the last one short, or false when none is left. */
bool
std_slice_Chunks_next(StdChunks *self, size_t size, size_t align, const void **chunk, size_t *chunk_len)
{
	size_t		chunksz;
	const void *snd;
	size_t		snd_len;

	if (self->v_len == 0)
		return false;
	chunksz = std_min_usize(self->v_len, self->chunk_size);
	std_slice_split_at_Unchecked(self->v, self->v_len, chunksz, size, align, chunk, chunk_len, &snd, &snd_len,
								 STD_SLICE_ITER_FILE, 1508, 37);
	self->v = snd;
	self->v_len = snd_len;
	return true;
}

/* std_slice_Chunks_size_hint is Chunks::size_hint: the exact number of chunks left, the last one short. */
StdSizeHint
std_slice_Chunks_size_hint(const StdChunks *self)
{
	size_t		n;

	if (self->v_len == 0)
		return (StdSizeHint)
	{
		0, true, 0
	};
	n = std_div_ceil_usize(self->v_len, self->chunk_size, STD_SLICE_ITER_FILE, 1519, 34);
	return (StdSizeHint)
	{
		n, true, n
	};
}

/*
 * std_slice_chunks_mut is <[T]>::chunks_mut, which is #[track_caller]: the
 * iterator over mutable chunks of a size, panicking at its caller for a size
 * of zero.
 */
StdChunksMut
std_slice_chunks_mut(void *v, size_t v_len, size_t chunk_size, const char *file, uint32 line, uint32 col)
{
	static const char text[] = "chunk size must be non-zero";
	StdChunksMut chunks = {v, v_len, chunk_size};

	if (!(chunk_size != 0))
		std_panic(file, line, col, text, sizeof(text) - 1);
	return chunks;
}

/*
 * std_slice_ChunksMut_next is ChunksMut::next where a crate codegens it: the
 * next chunk, the last one short, split off by the raw slice's split_at_mut,
 * whose assertion is at ChunksMut's call, or false when none is left.
 */
bool
std_slice_ChunksMut_next(StdChunksMut *self, size_t size, void **chunk, size_t *chunk_len)
{
	static const char text[] = "assertion failed: mid <= self.len()";
	size_t		sz;

	if (self->v_len == 0)
		return false;
	sz = std_min_usize(self->v_len, self->chunk_size);
	if (!(sz <= self->v_len))
		std_panic(STD_SLICE_ITER_FILE, 1684, 48, text, sizeof(text) - 1);
	*chunk = self->v;
	*chunk_len = sz;
	self->v = STD_PTR_AT(self->v, sz, size);
	self->v_len = std_wrapping_sub_usize(self->v_len, sz);
	return true;
}

/*
 * std_slice_windows is <[T]>::windows: an iterator over each run of size
 * elements, panicking at its caller for a size of zero.
 */
StdWindows
std_slice_windows(const void *v, size_t v_len, size_t size, const char *file, uint32 line, uint32 col)
{
	static const char text[] = "window size must be non-zero";
	StdWindows	windows = {v, v_len, size};

	if (size == 0)
		std_option_expect_failed(text, sizeof(text) - 1, file, line, col);
	return windows;
}

/*
 * std_slice_Windows_next is Windows::next: the next window, or false when
 * the slice left is shorter than one.
 */
bool
std_slice_Windows_next(StdWindows *self, size_t size, const void **window, size_t *window_len)
{
	if (self->size > self->v_len)
		return false;
	std_range_to_check(self->size, self->v_len, STD_SLICE_ITER_FILE, 1358, 35);
	*window = self->v;
	*window_len = self->size;
	std_range_from_check(1, self->v_len, STD_SLICE_ITER_FILE, 1359, 29);
	self->v = (const char *) self->v + size;
	self->v_len -= 1;
	return true;
}

/*
 * std_slice_as_chunks is <[T]>::as_chunks as core's own code runs it: the
 * slice's whole chunks of n elements and the remainder. n is the const
 * generic N, never zero, whose assert! rustc folds away in every instance.
 */
void
std_slice_as_chunks(const void *ptr, size_t len, size_t size, size_t align, size_t n, const void **chunks,
					size_t *chunks_len, const void **remainder, size_t *remainder_len, const char *file,
					uint32 line, uint32 col)
{
	size_t		len_rounded_down;
	size_t		multiple_of_n_len;

	len_rounded_down = STD_DIV_FOLDED(usize, len, n) * n;
	std_slice_split_at_unchecked_Unchecked(ptr, len, len_rounded_down, size, align, chunks, &multiple_of_n_len,
										   remainder, remainder_len, file, line, col);
	*chunks_len = multiple_of_n_len / n;
}

/*
 * std_slice_reverse_revswap is reverse's revswap: the first n elements of a
 * swapped with b's last n in reverse, each pair by mem::swap, whose
 * typed_swap_nonoverlapping is ptr::swap_nonoverlapping of one element.
 */
static void
std_slice_reverse_revswap(void *a, size_t a_len, void *b, size_t b_len, size_t n, size_t size, size_t align)
{
	void	   *a_n;
	size_t		a_n_len;
	void	   *a_rest;
	size_t		a_rest_len;
	void	   *b_n;
	size_t		b_n_len;
	void	   *b_rest;
	size_t		b_rest_len;
	size_t		i = 0;

	std_slice_split_at_mut(a, a_len, n, size, align, &a_n, &a_n_len, &a_rest, &a_rest_len, STD_SLICE_MOD_FILE, 1009, 28);
	std_slice_split_at_mut(b, b_len, n, size, align, &b_n, &b_n_len, &b_rest, &b_rest_len, STD_SLICE_MOD_FILE, 1010, 28);
	while (i < n)
	{
		std_index_check(i, a_n_len, STD_SLICE_MOD_FILE, 1014, 32);
		std_index_check(n - 1 - i, b_n_len, STD_SLICE_MOD_FILE, 1014, 43);
		std_ptr_swap_nonoverlapping((char *) a_n + i * size, (char *) b_n + (n - 1 - i) * size, 1, size, align,
									INTRINSICS_FILE, 2758, 14);
		i += 1;
	}
}

/*
 * std_slice_reverse is slice::reverse: the front half and the back half
 * made slices, then swapped pairwise from the outside in.
 */
void
std_slice_reverse(void *v, size_t len, size_t size, size_t align)
{
	size_t		half_len = len / 2;
	char	   *start = v;
	char	   *end = start + len * size;

	std_slice_from_raw_parts_mut_check(start, half_len, size, align, STD_SLICE_MOD_FILE, 989, 21);
	std_slice_from_raw_parts_mut_check(end - half_len * size, half_len, size, align, STD_SLICE_MOD_FILE, 990, 21);
	std_slice_reverse_revswap(start, half_len, end - half_len * size, half_len, half_len, size, align);
}

/*
 * std_slice_binary_search_by is <[T]>::binary_search_by over elements of
 * elem_size bytes: f's Ordering of the elements it probes, true with the
 * index of one f calls Equal in result, or false with where one would go.
 */
bool
std_slice_binary_search_by(const void *self, size_t len, size_t elem_size,
						   StdOrdering (*f) (void *captures, const void *elem), void *f_captures,
						   void (*f_drop) (void *), size_t *result)
{
	bool		ok = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		size_t		size = len;
		size_t		base = 0;
		StdOrdering cmp;

		if (size == 0)
			*result = 0;
		else
		{
			while (size > 1)
			{
				size_t		half = size / 2;
				size_t		mid = base + half;

				std_slice_get_unchecked_check(mid, len, STD_SLICE_MOD_FILE, 3005, 39);
				cmp = f(f_captures, (const char *) self + mid * elem_size);
				base = cmp == STD_ORDERING_GREATER ? base : mid;
				size -= half;
			}
			std_slice_get_unchecked_check(base, len, STD_SLICE_MOD_FILE, 3024, 35);
			cmp = f(f_captures, (const char *) self + base * elem_size);
			if (cmp == STD_ORDERING_EQUAL)
			{
				std_hint_assert_unchecked(base < len, STD_SLICE_MOD_FILE, 3027, 22);
				*result = base;
				ok = true;
			}
			else
			{
				size_t		result_2 = base + (size_t) (cmp == STD_ORDERING_LESS);

				std_hint_assert_unchecked(result_2 <= len, STD_SLICE_MOD_FILE, 3033, 22);
				*result = result_2;
			}
		}
		if (f_drop != NULL)
			f_drop(f_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (f_drop != NULL)
			f_drop(f_captures);
		std_unwind_resume(&unwind);
	}
	return ok;
}

/* std_slice_binary_search_closure_0_t is binary_search's closure's captures: the borrowed x and T's Ord. */
typedef struct std_slice_binary_search_closure_0_t
{
	const void *x;
	StdOrdering (*cmp) (const void *a, const void *b);
} std_slice_binary_search_closure_0_t;

/* std_slice_binary_search_closure_0 is binary_search's |p| p.cmp(x). */
static StdOrdering
std_slice_binary_search_closure_0(void *captures, const void *p)
{
	std_slice_binary_search_closure_0_t *c = (std_slice_binary_search_closure_0_t *) captures;

	return c->cmp(p, c->x);
}

/*
 * std_slice_binary_search is <[T]>::binary_search over elements of
 * elem_size bytes, T's Ord its cmp: binary_search_by of each element's
 * order against x, true with x's index when an element equals it.
 */
bool
std_slice_binary_search(const void *self, size_t len, size_t elem_size, const void *x,
						StdOrdering (*cmp) (const void *a, const void *b), size_t *result)
{
	std_slice_binary_search_closure_0_t captures = {x, cmp};

	return std_slice_binary_search_by(self, len, elem_size, std_slice_binary_search_closure_0, &captures, NULL,
									  result);
}

/*
 * std_bytes_starts_with is <[u8]>::starts_with: whether the bytes begin with
 * the needle's, compared after core's own check of the part it takes; an
 * empty needle compares no bytes, since an empty slice's pointer may be null
 * and memcmp's may not.
 */
bool
std_bytes_starts_with(const uint8 *self, size_t len, const uint8 *needle, size_t n)
{
	if (!(len >= n))
		return false;
	std_range_to_check(n, len, STD_SLICE_MOD_FILE, 2635, 43);
	return n == 0 || memcmp(needle, self, n) == 0;
}

/* std_slice_partition_point_closure_0_t is partition_point's closure's captures: the borrowed pred. */
typedef struct std_slice_partition_point_closure_0_t
{
	bool		(*pred) (void *captures, const void *elem);
	void	   *pred_captures;
} std_slice_partition_point_closure_0_t;

/* std_slice_partition_point_closure_0 is partition_point's |x| if pred(x) { Less } else { Greater }. */
static StdOrdering
std_slice_partition_point_closure_0(void *captures, const void *x)
{
	std_slice_partition_point_closure_0_t *c = (std_slice_partition_point_closure_0_t *) captures;

	return c->pred(c->pred_captures, x) ? STD_ORDERING_LESS : STD_ORDERING_GREATER;
}

/*
 * std_slice_partition_point is <[T]>::partition_point over elements of
 * elem_size bytes: the index of the first element pred is false for, pred
 * true for every element before it.
 */
size_t
std_slice_partition_point(const void *self, size_t len, size_t elem_size, bool (*pred) (void *captures, const void *elem),
						  void *pred_captures, void (*pred_drop) (void *))
{
	std_slice_partition_point_closure_0_t captures = {pred, pred_captures};
	size_t		i = 0;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_slice_binary_search_by(self, len, elem_size, std_slice_partition_point_closure_0, &captures, NULL, &i);
		if (pred_drop != NULL)
			pred_drop(pred_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (pred_drop != NULL)
			pred_drop(pred_captures);
		std_unwind_resume(&unwind);
	}
	return i;
}
