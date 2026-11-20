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
 * The C form of Rust's Vec<T> and String (the port text's section 9), a port
 * of alloc's Vec and RawVec at Rust 1.99.1: it allocates where they allocate
 * and panics with their messages at their locations.
 */
#ifndef DOLTGRES_STD_VEC_H
#define DOLTGRES_STD_VEC_H

#include "doltgres/std/alloc.h"
#include "doltgres/std/unwind.h"

/*
 * STD_RUST_LIBRARY is where Rust's panics place std's own source, the
 * toolchain's commit with the rest of the path in the platform's separator.
 */
#define STD_RUST_LIBRARY "/rustc/58cf8f9c35b355199727627d07ae34a4e2846d67/library"

/*
 * StdVec is a Vec<T> of elements of a size its callers pass, 0 for a
 * zero-sized type: its buffer, never NULL, its length, and its capacity.
 */
typedef struct StdVec
{
	void	   *ptr;
	size_t		len;
	size_t		cap;
} StdVec;

/* STD_VEC_NEW is Vec::new() of a type with a size, which allocates nothing. */
#define STD_VEC_NEW {STD_DANGLING, 0, 0}

/* STD_VEC_AT is the element at an index, which its caller has bounds-checked. */
#define STD_VEC_AT(v, T, i) (((T *) (v)->ptr)[(i)])

/* STD_VEC_AT_SIZE is the element at an index of elements of a size known at run time, which its caller has bounds-checked. */
#define STD_VEC_AT_SIZE(v, i, size) ((void *) ((char *) (v)->ptr + (i) * (size)))

/* StdString is a String: a StdVec of bytes, with no terminator. */
typedef StdVec StdString;

/*
 * StdVecDrain is vec::Drain: the vector it drains, the index of its next
 * element and of the range's end, where the tail starts and its length, and
 * the elements' size and drop glue, NULL for an element whose drop does
 * nothing.
 */
typedef struct StdVecDrain
{
	StdVec	   *vec;
	size_t		next;
	size_t		end;
	size_t		tail_start;
	size_t		tail_len;
	size_t		size;
	size_t		align;
	void		(*drop) (void *elem);
} StdVecDrain;

/* StdBoundTag is ops::Bound's variants. */
typedef enum StdBoundTag
{
	STD_BOUND_INCLUDED,
	STD_BOUND_EXCLUDED,
	STD_BOUND_UNBOUNDED,
} StdBoundTag;

/* StdBound is an ops::Bound<usize>: whether a range's end is included, excluded, or unbounded, and its index. */
typedef struct StdBound
{
	StdBoundTag tag;
	size_t		index;
} StdBound;

pg_noreturn extern void std_vec_panic_numbers(const char *file, uint32 line, uint32 col, const char *fmt, size_t a,
											  size_t b);
extern void std_vec_new(StdVec *v, size_t size, size_t align);
extern size_t std_vec_min_non_zero_cap(size_t size);
extern void std_vec_from_elem_zeroed(StdVec *v, size_t n, size_t size, size_t align);
extern void std_vec_u8_from_elem(StdVec *v, uint8 elem, size_t n);
extern bool std_vec_try_reserve(StdVec *v, size_t additional, size_t size, size_t align);
extern bool std_vec_try_reserve_exact(StdVec *v, size_t additional, size_t size, size_t align);
extern bool std_vec_pop(StdVec *v, void *out, size_t size);
extern void std_vec_swap_remove(StdVec *v, size_t index, void *out, size_t size, size_t align);
extern void std_vec_clone(StdVec *result, const StdVec *v, size_t size, size_t align);
extern void std_vec_dedup(StdVec *v, size_t size, size_t align, bool (*eq) (const void *self, const void *other),
						  void (*drop) (void *elem));
extern void std_vec_dedup_by(StdVec *v, size_t size, size_t align, bool (*same_bucket) (void *captures, void *a, void *b),
							 void *captures, void (*drop) (void *elem));
extern void std_slice_to_vec_clone(StdVec *result, const void *s, size_t len, size_t size, size_t align,
								   void (*clone) (const void *elem, void *result), void (*drop) (void *elem));
extern void std_vec_from_iter_in_place_map(StdVec *src, size_t src_size, size_t align, void (*src_drop) (void *elem),
										   size_t dst_size, void (*dst_drop) (void *elem),
										   void (*f) (void *elem, void *result), StdVec *result);
extern void std_vec_split_off(StdVec *v, size_t at, size_t size, size_t align, StdVec *result,
							  const char *file, uint32 line, uint32 col);
extern void std_vec_collect_first(StdVec *v, const void *elem, size_t lower,
								  size_t size, size_t align);
extern void std_vec_extend_one(StdVec *v, const void *elem, size_t lower,
							   size_t size, size_t align);
extern void std_vec_insert(StdVec *v, size_t index, const void *elem,
						   size_t size, size_t align, const char *file, uint32 line, uint32 col);
extern void std_vec_remove(StdVec *v, size_t index, void *out, size_t size, size_t align, const char *file,
						   uint32 line, uint32 col);
extern void std_ptr_drop_in_place_slice(void *ptr, size_t len, size_t size, void (*drop) (void *));
extern void std_vec_truncate(StdVec *v, size_t len, size_t size, void (*drop) (void *));
extern void std_vec_resize(StdVec *v, size_t new_len, const void *value, size_t size, size_t align);
extern void std_slice_concat(const void *slices, size_t n, size_t stride, size_t size, size_t align,
							 StdVec *result);
extern void std_str_join(const void *slices, size_t n, size_t stride, const char *sep, size_t sep_len,
						 StdString *result);
extern void std_slice_repeat(const void *slice, size_t len, size_t size, size_t align, size_t n, StdVec *result);
extern void std_str_repeat(const char *s, size_t len, size_t n, StdString *result);
extern void std_vec_clear(StdVec *v, size_t size, void (*drop) (void *));
extern StdVecDrain std_vec_drain_full(StdVec *v, size_t size, size_t align, void (*drop) (void *));
extern StdVecDrain std_vec_drain(StdVec *v, StdBound start, StdBound end, size_t size, size_t align,
								 void (*drop) (void *));
extern bool std_VecDrain_Iterator_next(void *self_, void *result);
extern void std_VecDrain_drop(void *self_);
extern bool std_vec_PartialEq_eq(const StdVec *self, const StdVec *other, size_t size,
								 bool (*eq) (const void *a, const void *b));
extern void std_string_truncate(StdString *self, size_t new_len, const char *file, uint32 line, uint32 col);
extern bool std_str_is_char_boundary(const char *self, size_t self_len, size_t index);

#define STD_VEC_DECLARE
#define STD_VEC_SUFFIX Checked
#include "doltgres/std/vec_template.h"
#define STD_VEC_DECLARE
#define STD_VEC_SUFFIX Unchecked
#include "doltgres/std/vec_template.h"

#ifdef DOLTGRES_TEST_HOST
extern void test_std_vec_tests_an_over_aligned_element_stays_aligned(void);
#endif


#include "doltgres/std/slice.h"

#endif							/* DOLTGRES_STD_VEC_H */
