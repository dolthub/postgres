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
 * The C form of alloc's VecDeque<T> (alloc/src/collections/vec_deque at Rust
 * 1.99.1) for a T of a size and alignment, with its drop where it has one:
 * a ring buffer of a RawVec's block and capacity, the physical index of the
 * first element, and the length. A crate's VecDeque is codegened in the
 * crate, so these are the Checked instances, which run the precondition
 * checks a crate's codegen keeps.
 */
#ifndef DOLTGRES_STD_VEC_DEQUE_H
#define DOLTGRES_STD_VEC_DEQUE_H

#include "doltgres/std/vec.h"

/* StdVecDeque is a VecDeque<T>: its RawVec's block and capacity, the physical index of its first element, and its length. */
typedef struct StdVecDeque
{
	void	   *ptr;
	size_t		cap;
	size_t		head;
	size_t		len;
} StdVecDeque;

/* STD_VEC_DEQUE_FILE is alloc's collections/vec_deque/mod.rs, where its checks report. */
#define STD_VEC_DEQUE_FILE STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "collections" STD_SEP "vec_deque" STD_SEP "mod.rs"

extern void std_vec_deque_new(StdVecDeque *self, size_t size, size_t align);
extern bool std_vec_deque_is_empty(const StdVecDeque *self);
extern const void *std_vec_deque_front_Checked(const StdVecDeque *self, size_t size);
extern bool std_vec_deque_pop_front_Checked(StdVecDeque *self, void *result, size_t size);
extern void std_vec_deque_reserve_Checked(StdVecDeque *self, size_t additional, size_t size, size_t align);
extern void *std_vec_deque_back_slot(StdVecDeque *self, size_t size);
extern void std_vec_deque_extend_vec_Checked(StdVecDeque *self, StdVec *iterator, size_t size, size_t align,
											 void (*drop) (void *));
extern void std_vec_deque_drop(StdVecDeque *self, size_t size, size_t align, void (*drop) (void *));

#endif							/* DOLTGRES_STD_VEC_DEQUE_H */
