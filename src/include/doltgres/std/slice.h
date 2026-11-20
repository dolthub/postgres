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
 * The C forms of core's slice methods the crates use (the port text's
 * section 10), over a slice's address, its length, and its elements' size,
 * each #[track_caller] form panicking at its caller.
 */
#ifndef DOLTGRES_STD_SLICE_H
#define DOLTGRES_STD_SLICE_H

#include "doltgres/std/cmp.h"
#include "doltgres/std/iter.h"

/* STD_SLICE_MOD_FILE is core's slice/mod.rs, where its methods' own checks report. */
#define STD_SLICE_MOD_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "slice" STD_SEP "mod.rs"

/* STD_SLICE_ITER_FILE is core's slice/iter.rs, where its iterators' own checks report. */
#define STD_SLICE_ITER_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "slice" STD_SEP "iter.rs"

/*
 * StdSliceSplit is core's slice::Split of a &[u8] by a predicate: the part
 * not yet split, the predicate and its captures, and whether it is
 * finished.
 */
typedef struct StdSliceSplit
{
	const uint8 *v;
	size_t		v_len;
	bool		(*pred) (void *captures, const uint8 *b);
	void	   *captures;
	bool		finished;
} StdSliceSplit;

/* STD_SLICE_INDEX_FILE is core's slice/index.rs, where a slice's checks that no caller's location reaches report. */
#define STD_SLICE_INDEX_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "slice" STD_SEP "index.rs"

extern void std_slice_range(StdBound start, StdBound end, size_t len, size_t *range_start, size_t *range_end);
extern void std_index_check(size_t index, size_t len, const char *file,
							uint32 line, uint32 col);
extern void std_range_check(size_t start, size_t end, size_t len,
							const char *file, uint32 line, uint32 col);
extern void std_range_from_check(size_t start, size_t len, const char *file,
								 uint32 line, uint32 col);
extern void std_range_to_check(size_t end, size_t len, const char *file,
							   uint32 line, uint32 col);
extern void std_range_inclusive_check(size_t start, size_t end, size_t len, const char *file, uint32 line,
									  uint32 col);
extern bool std_slice_split_at_checked(const void *ptr, size_t len, size_t mid, size_t size, size_t align,
									   const void **left, size_t *left_len, const void **right, size_t *right_len);
extern bool std_slice_split_last_chunk(const void *ptr, size_t len, size_t n, size_t size, size_t align,
									   const void **init, size_t *init_len, const void **last);
extern void std_slice_split_at_mut(void *ptr, size_t len, size_t mid, size_t size, size_t align, void **left,
								   size_t *left_len, void **right, size_t *right_len, const char *file,
								   uint32 line, uint32 col);

#define STD_SPLIT_DECLARE
#define STD_SPLIT_SUFFIX Checked
#include "doltgres/std/slice_split_template.h"
#define STD_SPLIT_DECLARE
#define STD_SPLIT_SUFFIX Unchecked
#include "doltgres/std/slice_split_template.h"
extern size_t std_iter_step_by_range_usize(size_t start, size_t end, size_t step);
extern uint64 std_iter_step_by_range_u64(uint64 start, uint64 end, size_t step);
extern void std_slice_chunks_check(size_t chunk_size, const char *file, uint32 line, uint32 col);
extern StdSliceSplit std_slice_split_u8(const uint8 *v, size_t len, bool (*pred) (void *captures, const uint8 *b),
										void *captures);
extern bool std_SliceSplit_next(StdSliceSplit *self, const uint8 **piece, size_t *piece_len);

/* StdSliceAlignTo is align_to's three slices: the Ts before the aligned Us, the Us, and the Ts after. */
typedef struct StdSliceAlignTo
{
	const void *head;
	size_t		head_len;
	const void *body;
	size_t		body_len;
	const void *tail;
	size_t		tail_len;
} StdSliceAlignTo;

/*
 * StdSliceStepBy is StepBy<slice::Iter>: the slice left, step - 1, and
 * whether the first item is still to be taken.
 */
typedef struct StdSliceStepBy
{
	const void *v;
	size_t		len;
	size_t		step_minus_one;
	bool		first_take;
} StdSliceStepBy;

/* StdChunks is slice::Chunks: the slice left and the chunk size. */
typedef struct StdChunks
{
	const void *v;
	size_t		v_len;
	size_t		chunk_size;
} StdChunks;

/* StdChunksMut is slice::ChunksMut: the slice left and the chunk size. */
typedef struct StdChunksMut
{
	void	   *v;
	size_t		v_len;
	size_t		chunk_size;
} StdChunksMut;

/* StdWindows is slice::Windows: the slice left and the window's size. */
typedef struct StdWindows
{
	const void *v;
	size_t		v_len;
	size_t		size;
} StdWindows;

extern void std_slice_align_to(const void *ptr, size_t len, size_t t_size, size_t t_align, size_t u_size,
							   size_t u_align, StdSliceAlignTo *result);
extern StdSliceStepBy std_slice_Iter_step_by(const void *v, size_t len, size_t step);
extern bool std_SliceStepBy_Iterator_next(StdSliceStepBy *self, size_t size, const void **result);
extern StdChunks std_slice_chunks(const void *v, size_t v_len, size_t chunk_size);
extern StdChunksMut std_slice_chunks_mut(void *v, size_t v_len, size_t chunk_size, const char *file, uint32 line,
										 uint32 col);
extern bool std_slice_ChunksMut_next(StdChunksMut *self, size_t size, void **chunk, size_t *chunk_len);
extern bool std_slice_Chunks_next(StdChunks *self, size_t size, size_t align, const void **chunk,
								  size_t *chunk_len);
extern StdSizeHint std_slice_Chunks_size_hint(const StdChunks *self);
extern void std_slice_as_chunks(const void *ptr, size_t len, size_t size, size_t align, size_t n,
								const void **chunks, size_t *chunks_len, const void **remainder,
								size_t *remainder_len, const char *file, uint32 line, uint32 col);
extern void std_slice_reverse(void *v, size_t len, size_t size, size_t align);
extern StdWindows std_slice_windows(const void *v, size_t v_len, size_t size, const char *file, uint32 line,
									uint32 col);
extern bool std_slice_Windows_next(StdWindows *self, size_t size, const void **window, size_t *window_len);
extern bool std_slice_binary_search_by(const void *self, size_t len, size_t elem_size,
									   StdOrdering (*f) (void *captures, const void *elem), void *f_captures,
									   void (*f_drop) (void *), size_t *result);
extern bool std_slice_binary_search(const void *self, size_t len, size_t elem_size, const void *x,
									StdOrdering (*cmp) (const void *a, const void *b), size_t *result);
extern bool std_bytes_starts_with(const uint8 *self, size_t len, const uint8 *needle, size_t n);
extern size_t std_slice_partition_point(const void *self, size_t len, size_t elem_size,
										bool (*pred) (void *captures, const void *elem), void *pred_captures,
										void (*pred_drop) (void *));

#endif							/* DOLTGRES_STD_SLICE_H */
