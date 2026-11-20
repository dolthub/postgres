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
 * The C form of a generic parameter bounded by Iterator or IntoIterator
 * (the port text's sections 8 and 10): the trait's table, its next and the
 * implementer's drop glue, and the implementer, which the callee takes and
 * drops as Rust drops a moved iterator.
 */
#ifndef DOLTGRES_STD_ITER_H
#define DOLTGRES_STD_ITER_H

#include "doltgres/std/vec.h"

/* StdSizeHint is size_hint's result: the lower bound, and the upper bound when there is one. */
typedef struct StdSizeHint
{
	size_t		lower;
	bool		has_upper;
	size_t		upper;
} StdSizeHint;

/*
 * StdIteratorOps is Iterator's table: next writes the item and returns
 * true, or returns false for None; size_hint bounds the items left.
 */
typedef struct StdIteratorOps
{
	bool		(*next) (void *self, void *result);
	StdSizeHint (*size_hint) (const void *self);

	/*
	 * drop is the implementer's drop glue, NULL for one whose drop does
	 * nothing.
	 */
	void		(*drop) (void *self);
} StdIteratorOps;

/* StdIterator is a value of a type that implements Iterator: its table and the value. */
typedef struct StdIterator
{
	const StdIteratorOps *ops;
	void	   *self;
} StdIterator;

/* std_iterator_drop drops an iterator a callee took. */
static inline void
std_iterator_drop(StdIterator *iter)
{
	if (iter->ops->drop != NULL)
		iter->ops->drop(iter->self);
}

/*
 * StdVecIntoIter is vec::IntoIter: the vector it owns, the index of its
 * next element, and its elements' size and drop glue, NULL for one whose
 * drop does nothing.
 */
typedef struct StdVecIntoIter
{
	StdVec		buf;
	size_t		next;
	size_t		size;
	size_t		align;
	void		(*drop) (void *elem);
} StdVecIntoIter;

/*
 * StdArrayIntoIter is array::IntoIter: the array it owns, of len elements of
 * a size, and the range of those alive in it.
 */
typedef struct StdArrayIntoIter
{
	void	   *data;
	size_t		len;
	size_t		size;
	size_t		start;
	size_t		end;
} StdArrayIntoIter;

extern const StdIteratorOps STD_VEC_INTO_ITER_ITERATOR_OPS;
extern const StdIteratorOps STD_ARRAY_INTO_ITER_ITERATOR_OPS;
extern bool std_ArrayIntoIter_Iterator_next(void *self_, void *result);
extern StdSizeHint std_ArrayIntoIter_Iterator_size_hint(const void *self_);

/* StdRangeUsize is a Range<usize>: its next value and its end. */
typedef struct StdRangeUsize
{
	size_t		start;
	size_t		end;
} StdRangeUsize;

extern StdSizeHint std_Iterator_size_hint(const void *self);
extern StdSizeHint std_FlattenCompat_size_hint(StdSizeHint iter, size_t fixed_size);

/*
 * StdVecSplice is vec::Splice of a TrustedLen iterator: the drain of the
 * range it replaces, and the iterator of what replaces it, which it owns.
 */
typedef struct StdVecSplice
{
	StdVecDrain drain;
	StdIterator replace_with;
} StdVecSplice;

extern StdVecSplice std_vec_splice(StdVec *v, StdBound start, StdBound end, size_t size, size_t align,
								   void (*drop) (void *), StdIterator replace_with);
extern void std_VecSplice_drop(StdVecSplice *self);
extern StdVecIntoIter std_Vec_IntoIterator_into_iter(StdVec *v, size_t size, size_t align, void (*drop) (void *elem));
extern bool std_VecIntoIter_Iterator_next(void *self, void *result);
extern StdSizeHint std_VecIntoIter_Iterator_size_hint(const void *self);
extern size_t std_VecIntoIter_len(const StdVecIntoIter *self);
extern void std_vec_extend_trusted_reserve(StdVec *v, StdSizeHint hint, size_t size, size_t align);
extern void std_vec_with_capacity_trusted(StdVec *v, StdSizeHint hint, size_t size, size_t align);
extern bool std_array_IntoIter_next(size_t *start, size_t end, size_t len, size_t *index);
extern void std_array_IntoIter_fold_begin(size_t start, size_t end);
extern void std_array_Drain_drop(const void *next, const void *end, size_t size);
extern void std_array_Guard_drop(size_t initialized, size_t len);
extern bool std_array_IntoIter_fold_next(size_t *start, size_t end, size_t len, size_t *index);
extern void std_VecIntoIter_drop(void *self);
extern void std_Iterator_unzip_VecIntoIter_Checked(StdVecIntoIter *iter, size_t a_offset, size_t a_size,
												   size_t a_align, size_t b_offset, size_t b_size, size_t b_align,
												   StdVec *a, StdVec *b);
extern void std_Iterator_unzip_CopiedSliceIter_Checked(const void *v, size_t len, size_t size, size_t a_offset,
													   size_t a_size, size_t a_align, size_t b_offset, size_t b_size,
													   size_t b_align, StdVec *a, StdVec *b);

#endif							/* DOLTGRES_STD_ITER_H */
