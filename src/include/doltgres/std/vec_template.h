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
 * The template of Vec's and String's growth, RawVec's and Global's, and
 * Box::new_uninit_in's allocation, whose precondition checks run where they
 * are codegened (the port text's section 3), ported from
 * alloc/src/raw_vec/mod.rs, alloc/src/alloc.rs, alloc/src/vec/mod.rs,
 * alloc/src/string.rs, and alloc/src/boxed.rs at Rust 1.99.1. A crate's
 * vector is codegened in the crate, checked in the cassert build; std's own
 * non-inline code's (formatting, paths, I/O) is not. vec.c, which includes
 * it, defines the checks and the files they name. STD_VEC_DECLARE and
 * STD_VEC_DEFINE choose the declarations and definitions, STD_VEC_UB_CHECKS
 * is 1 for the checked instance and 0 for the other, and STD_VEC_SUFFIX names
 * the instance; every parameter is undefined at the end.
 */

/* STD_VEC_CAT pastes two tokens after expanding them. */
#define STD_VEC_CAT_(a, b) a##b
#define STD_VEC_CAT(a, b) STD_VEC_CAT_(a, b)

/* STD_VEC_NAME names the instance's functions. */
#define STD_VEC_NAME(name) STD_VEC_CAT(name##_, STD_VEC_SUFFIX)

#ifdef STD_VEC_DECLARE
extern void *STD_VEC_NAME(std_box_new_uninit) (size_t size, size_t align);
extern void *STD_VEC_NAME(std_box_new_uninit_layout) (size_t size, size_t align);
extern void STD_VEC_NAME(std_vec_finish_grow) (StdVec *v, size_t cap, size_t size, size_t align);
extern void STD_VEC_NAME(std_vec_grow_amortized) (StdVec *v, size_t additional, size_t size, size_t align);
extern void STD_VEC_NAME(std_RawVec_reserve) (StdVec *v, size_t len, size_t additional, size_t size, size_t align);
extern void STD_VEC_NAME(std_vec_with_capacity) (StdVec *v, size_t cap, size_t size, size_t align);
extern void STD_VEC_NAME(std_vec_reserve) (StdVec *v, size_t additional, size_t size, size_t align);
extern void STD_VEC_NAME(std_vec_reserve_exact) (StdVec *v, size_t additional, size_t size, size_t align);
extern void STD_VEC_NAME(std_vec_shrink_to_fit) (StdVec *v, size_t size, size_t align);
extern void *STD_VEC_NAME(std_vec_into_boxed_slice) (StdVec *v, size_t size, size_t align, size_t *len);
extern void STD_VEC_NAME(std_vec_set_len) (StdVec *v, size_t new_len);
extern void STD_VEC_NAME(std_vec_free) (StdVec *v, size_t size, size_t align);
extern void STD_VEC_NAME(std_vec_drop) (StdVec *v, size_t size, size_t align, void (*drop) (void *));
extern void STD_VEC_NAME(std_vec_retain) (StdVec *v, size_t size, size_t align, void (*drop) (void *),
										  bool (*f) (void *captures, const void *elem), void *captures);
extern void STD_VEC_NAME(std_vec_push) (StdVec *v, const void *elem, size_t size, size_t align);
extern void STD_VEC_NAME(std_vec_u8_push) (StdVec *v, uint8 byte);
extern void STD_VEC_NAME(std_vec_extend_from_slice) (StdVec *v, const void *elems, size_t n, size_t size,
													 size_t align);
extern void STD_VEC_NAME(std_vec_extend_from_within) (StdVec *v, size_t start, size_t end, size_t size,
													  size_t align);
extern void STD_VEC_NAME(std_string_push) (StdString *s, uint32 ch);
extern void STD_VEC_NAME(std_string_push_str) (StdString *s, const char *str, size_t len);
extern void STD_VEC_NAME(std_string_from) (StdString *s, const char *str, size_t len);
extern void STD_VEC_NAME(std_slice_to_vec) (StdVec *v, const void *s, size_t len, size_t size, size_t align);
extern void STD_VEC_NAME(std_box_slice_into_vec) (StdVec *v, void *ptr, size_t n, size_t size);
#endif

#ifdef STD_VEC_DEFINE

/*
 * std_Global_alloc_impl is Global's alloc_impl: a dangling block for a zero
 * size, else the global allocator's block, zeroed when asked, or NULL for
 * AllocError.
 */
static void *
STD_VEC_NAME(std_Global_alloc_impl) (size_t size, size_t align, bool zeroed)
{
	void	   *raw_ptr;

	if (size == 0)
	{
		raw_ptr = STD_DANGLING_FOR(align);
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_NonNull_new_unchecked_check(raw_ptr, STD_PTR_NON_NULL_FILE, 1395, 18);
#endif
		return raw_ptr;
	}
	raw_ptr = zeroed ? std_global_alloc_zeroed(size, align) : std_global_alloc(size, align);
	if (raw_ptr == NULL)
		return NULL;
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_vec_allocated_checks(raw_ptr);
#endif
	return raw_ptr;
}

/*
 * std_Global_grow_impl is Global's grow_impl_runtime: a block grown from an
 * old size and alignment to a new one, by allocation for an empty block, by
 * realloc for the same alignment (the new bytes zeroed when asked), and
 * otherwise by a new block the old bytes are copied into before the old one
 * is freed; NULL for AllocError, the old block kept.
 */
static void *
STD_VEC_NAME(std_Global_grow_impl) (void *ptr, size_t old_size, size_t old_align, size_t new_size, size_t new_align,
									bool zeroed)
{
	void	   *raw_ptr;
	void	   *new_ptr;

	if (old_size == 0)
		return STD_VEC_NAME(std_Global_alloc_impl) (new_size, new_align, zeroed);
	if (old_align == new_align)
	{
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_hint_assert_unchecked(new_size >= old_size, ALLOC_FILE, 349, 17);
#endif
		raw_ptr = std_global_realloc(ptr, old_size, new_align, new_size);
		if (raw_ptr == NULL)
			return NULL;
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_NonNull_new_unchecked_check(raw_ptr, STD_PTR_NON_NULL_FILE, 282, 27);
		if (zeroed)
			std_ptr_write_bytes((uint8 *) raw_ptr + old_size, 0, new_size - old_size, 1, 1);
		std_NonNull_new_unchecked_check(raw_ptr, STD_PTR_NON_NULL_FILE, 1395, 18);
#else
		if (zeroed)
			memset((uint8 *) raw_ptr + old_size, 0, new_size - old_size);
#endif
		return raw_ptr;
	}
	new_ptr = STD_VEC_NAME(std_Global_alloc_impl) (new_size, new_align, zeroed);
	if (new_ptr == NULL)
		return NULL;
	STD_VEC_CAT(std_ptr_copy_nonoverlapping_, STD_VEC_SUFFIX) (ptr, new_ptr, old_size, 1, 1);
	std_free(ptr, old_size, old_align);
	return new_ptr;
}

/*
 * std_box_new_uninit is Box::new_uninit_in with Global: a block for a value
 * of a size and alignment, a dangling one for a zero size, as
 * try_new_uninit_in's allocate and from_raw_in's Unique::new_unchecked make
 * it; an allocation that fails is handle_alloc_error's.
 */
void *
STD_VEC_NAME(std_box_new_uninit) (size_t size, size_t align)
{
	void	   *ptr;

	if (size == 0)
		ptr = STD_DANGLING_FOR(align);
	else
	{
		ptr = STD_VEC_NAME(std_Global_alloc_impl) (size, align, false);
		if (ptr == NULL)
			std_handle_alloc_error(size, align);
	}
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_NonNull_new_unchecked_check(ptr, STD_PTR_UNIQUE_FILE, 88, 36);
#endif
	return ptr;
}

/*
 * std_box_new_uninit_layout is box_new_uninit: Global's block for a layout,
 * or handle_alloc_error's report when the allocation fails.
 */
void *
STD_VEC_NAME(std_box_new_uninit_layout) (size_t size, size_t align)
{
	void	   *ptr = STD_VEC_NAME(std_Global_alloc_impl) (size, align, false);

	if (ptr == NULL)
		std_handle_alloc_error(size, align);
	return ptr;
}

/*
 * std_vec_finish_grow moves a vector's buffer to a capacity, allocating it the first
 * time and growing it through Global after, as RawVec's finish_grow does; a
 * size past isize::MAX bytes is a capacity overflow, and an allocation that
 * fails is handle_alloc_error's, as grow_amortized's handle_error makes it.
 */
void
STD_VEC_NAME(std_vec_finish_grow) (StdVec *v, size_t cap, size_t size, size_t align)
{
	void	   *ptr;

	if (cap > std_vec_max_size_for_align(align) / size)
		std_vec_capacity_overflow();
	if (v->cap != 0)
	{
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_vec_current_memory_checks(v, size, align);
		std_hint_assert_unchecked(true, RAW_VEC_FILE, 556, 17);
#endif
		ptr = STD_VEC_NAME(std_Global_grow_impl) (v->ptr, v->cap * size, align, cap * size, align, false);
	}
	else
		ptr = STD_VEC_NAME(std_Global_alloc_impl) (cap * size, align, false);
	if (ptr == NULL)
		std_handle_alloc_error(cap * size, align);
	v->ptr = ptr;
	v->cap = cap;
}

/*
 * std_RawVec_grow_amortized is RawVec's growth for room for `additional`
 * more elements past `len`: twice the capacity or what is needed, whichever
 * is more, and at least the smallest capacity it allocates.
 */
static void
STD_VEC_NAME(std_RawVec_grow_amortized) (StdVec *v, size_t len, size_t additional, size_t size, size_t align)
{
	size_t		required;
	size_t		cap;

	if (size == 0 || additional > SIZE_MAX - len)
		std_vec_capacity_overflow();
	required = len + additional;
	cap = std_max_usize(v->cap * 2, required);
	cap = std_max_usize(std_vec_min_non_zero_cap(size), cap);
	STD_VEC_NAME(std_vec_finish_grow) (v, cap, size, align);
}

/* std_vec_grow_amortized is RawVec's growth for room for `additional` more elements past the vector's length. */
void
STD_VEC_NAME(std_vec_grow_amortized) (StdVec *v, size_t additional, size_t size, size_t align)
{
	STD_VEC_NAME(std_RawVec_grow_amortized) (v, v->len, additional, size, align);
}

/* std_RawVec_reserve is RawVec::reserve, which grows by RawVec's rule when there is no room for `additional` past `len`. */
void
STD_VEC_NAME(std_RawVec_reserve) (StdVec *v, size_t len, size_t additional, size_t size, size_t align)
{
	if (v->cap - len < additional)
		STD_VEC_NAME(std_RawVec_grow_amortized) (v, len, additional, size, align);
}

/*
 * std_vec_with_capacity is Vec::with_capacity, which allocates `cap` exactly,
 * as RawVecInner's try_allocate_in does: a size past isize::MAX bytes is a
 * capacity overflow, and a size of zero allocates nothing.
 */
void
STD_VEC_NAME(std_vec_with_capacity) (StdVec *v, size_t cap, size_t size, size_t align)
{
	std_vec_new(v, size, align);
	if (size != 0 && cap > std_vec_max_size_for_align(align) / size)
		std_vec_capacity_overflow();
	if (size != 0 && cap != 0)
	{
		v->ptr = STD_VEC_NAME(std_Global_alloc_impl) (cap * size, align, false);
		if (v->ptr == NULL)
			std_handle_alloc_error(cap * size, align);
		v->cap = cap;
	}
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_hint_assert_unchecked(!(cap > v->cap), RAW_VEC_FILE, 439, 21);
#endif
}

/* std_vec_set_len is Vec::set_len: the length set, its precondition checked at alloc's own location. */
void
STD_VEC_NAME(std_vec_set_len) (StdVec *v, size_t new_len)
{
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	if (!(new_len <= v->cap))
		STD_PTR_VIOLATED("Vec::set_len requires that new_len <= capacity()", VEC_FILE, 2218, 9);
#endif
	v->len = new_len;
}

/*
 * std_vec_free is the drop of a vector's buffer, after its caller dropped the
 * elements that own anything.
 */
void
STD_VEC_NAME(std_vec_free) (StdVec *v, size_t size, size_t align)
{
	if (size != 0)
	{
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		if (v->cap != 0)
			std_vec_current_memory_checks(v, size, align);
#endif
		std_free(v->ptr, v->cap * size, align);
	}
}

/*
 * std_vec_drop is Vec<T>'s drop for a T whose drop does something: its
 * elements' drops in order, as the drop of its slice runs them, the rest
 * still dropped should one panic, then its buffer's free, which a panic in an
 * element's drop still runs, as RawVec's drop does.
 */
void
STD_VEC_NAME(std_vec_drop) (StdVec *v, size_t size, size_t align, void (*drop) (void *))
{
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_ptr_drop_in_place_slice(v->ptr, v->len, size, drop);
		STD_VEC_NAME(std_vec_free) (v, size, align);
		std_unwind_pop(&unwind);
	}
	else
	{
		STD_VEC_NAME(std_vec_free) (v, size, align);
		std_unwind_resume(&unwind);
	}
}

/*
 * STD_VEC_RETAIN_FRAME_0_T is retain_mut's state across its pad: the
 * vector, the predicate, and its PanicGuard's read and write positions.
 */
#define STD_VEC_RETAIN_FRAME_0_T STD_VEC_NAME(std_vec_retain_frame_0_t)
typedef struct STD_VEC_RETAIN_FRAME_0_T
{
	StdVec	   *v;
	size_t		size;
	size_t		align;
	void		(*drop) (void *);
	bool		(*f) (void *captures, const void *elem);
	void	   *captures;
	size_t		original_len;
	size_t		read;
	size_t		write;
	bool		guarded;
}			STD_VEC_RETAIN_FRAME_0_T;

/* std_vec_retain_body_0 is retain_mut's body: the run of kept elements, then the backshift past the first dropped one. */
static void
STD_VEC_NAME(std_vec_retain_body_0) (void *frame_)
{
	STD_VEC_RETAIN_FRAME_0_T *frame = frame_;
	void	   *base = frame->v->ptr;

	for (;;)
	{
		void	   *cur;

#if STD_VEC_UB_CHECKS
		std_slice_get_unchecked_mut_check(frame->read, frame->v->len, VEC_FILE, 2557, 37);
#endif
		cur = STD_PTR_AT(base, frame->read, frame->size);
		if (!frame->f(frame->captures, cur))
			break;
		frame->read = frame->read + 1;
		if (frame->read == frame->original_len)
			return;
	}
	frame->write = frame->read;
	frame->read = frame->read + 1;
	frame->guarded = true;
	if (frame->drop != NULL)
		frame->drop(STD_PTR_AT(base, frame->write, frame->size));
	while (frame->read < frame->original_len)
	{
		void	   *cur = STD_PTR_AT(base, frame->read, frame->size);

		if (!frame->f(frame->captures, cur))
		{
			frame->read = frame->read + 1;
			if (frame->drop != NULL)
				frame->drop(cur);
		}
		else
		{
			STD_VEC_CAT(std_ptr_copy_nonoverlapping_, STD_VEC_SUFFIX) (cur, STD_PTR_AT(base, frame->write, frame->size), 1,
																	   frame->size, frame->align);
			frame->write = frame->write + 1;
			frame->read = frame->read + 1;
		}
	}
	STD_VEC_NAME(std_vec_set_len) (frame->v, frame->write);
	frame->guarded = false;
}

/* std_vec_retain_pad_0 is retain_mut's pad: PanicGuard's drop, which shifts the elements not yet read back over the hole and sets the length. */
static void
STD_VEC_NAME(std_vec_retain_pad_0) (void *frame_)
{
	STD_VEC_RETAIN_FRAME_0_T *frame = frame_;
	void	   *base;
	size_t		remaining;

	if (!frame->guarded)
		return;
	base = frame->v->ptr;
	remaining = frame->original_len - frame->read;
	std_ptr_copy(STD_PTR_AT(base, frame->read, frame->size), STD_PTR_AT(base, frame->write, frame->size), remaining, frame->size,
				 frame->align);
	STD_VEC_NAME(std_vec_set_len) (frame->v, frame->write + remaining);
}

/*
 * std_vec_retain is Vec::retain through retain_mut: the elements for which
 * f is false dropped in order and the rest kept in place, the vector whole
 * should f or a drop panic, as its PanicGuard keeps it. drop is the
 * element's drop, NULL for an element whose drop does nothing.
 */
void
STD_VEC_NAME(std_vec_retain) (StdVec *v, size_t size, size_t align, void (*drop) (void *),
							  bool (*f) (void *captures, const void *elem), void *captures)
{
	STD_VEC_RETAIN_FRAME_0_T frame = {.v = v, .size = size, .align = align, .drop = drop, .f = f,
	.captures = captures, .original_len = v->len};

	if (frame.original_len == 0)
		return;
	std_landing(&frame, STD_VEC_NAME(std_vec_retain_body_0), STD_VEC_NAME(std_vec_retain_pad_0));
}

/* std_vec_reserve is Vec::reserve, RawVec::reserve past the vector's length. */
void
STD_VEC_NAME(std_vec_reserve) (StdVec *v, size_t additional, size_t size, size_t align)
{
	STD_VEC_NAME(std_RawVec_reserve) (v, v->len, additional, size, align);
}

/*
 * std_RawVec_reserve_exact is RawVec::reserve_exact: when `additional` more
 * elements past `len` don't fit, grow_exact's capacity of exactly their sum,
 * a zero-sized element's or an overflowing sum's being a capacity overflow.
 */
static void
STD_VEC_NAME(std_RawVec_reserve_exact) (StdVec *v, size_t len, size_t additional, size_t size, size_t align)
{
	if (additional > v->cap - len)
	{
		if (size == 0 || additional > SIZE_MAX - len)
			std_vec_capacity_overflow();
		STD_VEC_NAME(std_vec_finish_grow) (v, len + additional, size, align);
	}
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_hint_assert_unchecked(!(additional > v->cap - len), RAW_VEC_FILE, 730, 13);
#endif
}

/* std_vec_reserve_exact is Vec::reserve_exact, RawVec::reserve_exact past the vector's length. */
void
STD_VEC_NAME(std_vec_reserve_exact) (StdVec *v, size_t additional, size_t size, size_t align)
{
	STD_VEC_NAME(std_RawVec_reserve_exact) (v, v->len, additional, size, align);
}

/*
 * std_RawVec_shrink_to_fit is RawVec::shrink_to_fit: shrink's assert, then
 * for a buffer it has, a free and a dangling pointer for a capacity of zero,
 * else Global's shrink to the smaller size (realloc for the one alignment),
 * whose failure is handle_alloc_error's for the new layout.
 */
static void
STD_VEC_NAME(std_RawVec_shrink_to_fit) (StdVec *v, size_t cap, size_t size, size_t align)
{
	size_t		old_size;
	size_t		new_size;
	void	   *ptr;

	if (!(cap <= (size == 0 ? SIZE_MAX : v->cap)))
	{
		static const char text[] = "Tried to shrink to a larger capacity";

		std_panic(RAW_VEC_FILE, 811, 9, text, sizeof(text) - 1);
	}
	if (size == 0 || v->cap == 0)
		return;
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_vec_current_memory_checks(v, size, align);
#endif
	old_size = v->cap * size;
	if (cap == 0)
	{
		std_free(v->ptr, old_size, align);
		v->ptr = STD_DANGLING_FOR(align);
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_NonNull_new_unchecked_check(v->ptr, STD_PTR_UNIQUE_FILE, 88, 36);
#endif
		v->cap = 0;
		return;
	}
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_usize_unchecked_mul_check(size, cap, RAW_VEC_FILE, 848, 51);
	std_Layout_from_size_align_unchecked_check(size * cap, align, RAW_VEC_FILE, 849, 34);
	std_hint_assert_unchecked(size * cap <= old_size, ALLOC_FILE, 398, 17);
#endif
	new_size = size * cap;
	ptr = std_global_realloc(v->ptr, old_size, align, new_size);
	if (ptr == NULL)
		std_handle_alloc_error(new_size, align);
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_NonNull_new_unchecked_check(ptr, STD_PTR_NON_NULL_FILE, 282, 27);
	std_NonNull_new_unchecked_check(ptr, STD_PTR_NON_NULL_FILE, 1395, 18);
#endif
	v->ptr = ptr;
	v->cap = cap;
}

/* std_vec_shrink_to_fit is Vec::shrink_to_fit: RawVec's, only when the capacity is past the length. */
void
STD_VEC_NAME(std_vec_shrink_to_fit) (StdVec *v, size_t size, size_t align)
{
	if ((size == 0 ? SIZE_MAX : v->cap) > v->len)
		STD_VEC_NAME(std_RawVec_shrink_to_fit) (v, v->len, size, align);
}

/*
 * std_vec_into_boxed_slice is Vec::into_boxed_slice: the buffer shrunk to the
 * length, then the Box<[T]> of its pointer and length, which Box::from_raw_in
 * makes through Unique::new_unchecked; the vector is moved out.
 */
void *
STD_VEC_NAME(std_vec_into_boxed_slice) (StdVec *v, size_t size, size_t align, size_t *len)
{
	STD_VEC_NAME(std_vec_shrink_to_fit) (v, size, align);
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_NonNull_new_unchecked_check(v->ptr, STD_PTR_UNIQUE_FILE, 88, 36);
#endif
	*len = v->len;
	return v->ptr;
}

/* std_vec_push is Vec::push, growing by one step of RawVec's rule when full. */
void
STD_VEC_NAME(std_vec_push) (StdVec *v, const void *elem, size_t size, size_t align)
{
	if (v->len == v->cap)
		STD_VEC_NAME(std_vec_grow_amortized) (v, 1, size, align);
	if (size != 0)
		memcpy((char *) v->ptr + v->len * size, elem, size);
	v->len++;
}

/*
 * std_vec_u8_push is Vec<u8>::push: as std_vec_push of a byte, growing
 * through RawVec<u8>::grow_one's instance in alloc, which every crate links
 * rather than codegens, so its growth is std's, unchecked.
 */
void
STD_VEC_NAME(std_vec_u8_push) (StdVec *v, uint8 byte)
{
	if (v->len == v->cap)
		std_vec_grow_amortized_Unchecked(v, 1, 1, 1);
	((uint8 *) v->ptr)[v->len] = byte;
	v->len++;
}

/* std_vec_extend_from_slice is Vec::extend_from_slice: a reserve, then a copy. */
void
STD_VEC_NAME(std_vec_extend_from_slice) (StdVec *v, const void *elems, size_t n, size_t size, size_t align)
{
	STD_VEC_NAME(std_vec_reserve) (v, n, size, align);
	STD_VEC_CAT(std_ptr_copy_nonoverlapping_, STD_VEC_SUFFIX) (elems, (char *) v->ptr + v->len * size, n, size, align);
	v->len += n;
}

/*
 * std_vec_extend_from_within is Vec::extend_from_within over a Range<usize>
 * of a Copy T: slice::range's checks of the range within the vector, a
 * reserve, then spec_extend_from_within's copy of the range to the vector's
 * end.
 */
void
STD_VEC_NAME(std_vec_extend_from_within) (StdVec *v, size_t start, size_t end, size_t size, size_t align)
{
	StdBound	start_bound = {STD_BOUND_INCLUDED, start};
	StdBound	end_bound = {STD_BOUND_EXCLUDED, end};
	size_t		range_start;
	size_t		range_end;
	size_t		count;

	std_slice_range(start_bound, end_bound, v->len, &range_start, &range_end);
	STD_VEC_NAME(std_vec_reserve) (v, std_wrapping_sub_usize(range_end, range_start), size, align);
	count = std_wrapping_sub_usize(range_end, range_start);
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_slice_from_raw_parts_mut_check(v->ptr, v->len, size, align, VEC_FILE, 3392, 31);
	std_slice_from_raw_parts_mut_check(STD_PTR_AT(v->ptr, v->len, size), std_wrapping_sub_usize(v->cap, v->len), size,
									   align, VEC_FILE, 3393, 25);
	std_slice_get_unchecked_range_check(range_start, range_end, v->len, VEC_FILE, 3821, 40);
#endif
	STD_VEC_CAT(std_ptr_copy_nonoverlapping_, STD_VEC_SUFFIX) (STD_PTR_AT(v->ptr, range_start, size),
															   STD_PTR_AT(v->ptr, v->len, size), count, size, align);
	v->len = std_wrapping_add_usize(v->len, count);
}

/*
 * std_string_push is String::push: room reserved for the char's UTF-8
 * length, its bytes encoded at the end, and the length set past them.
 */
void
STD_VEC_NAME(std_string_push) (StdString *s, uint32 ch)
{
	size_t		len = s->len;
	size_t		ch_len = std_char_len_utf8(ch);

	STD_VEC_NAME(std_vec_reserve) (s, ch_len, 1, 1);
	(void) std_fmt_encode_utf8(ch, STD_PTR_AT(s->ptr, len, 1));
	STD_VEC_NAME(std_vec_set_len) (s, std_wrapping_add_usize(len, ch_len));
}

/* std_string_push_str is String::push_str. */
void
STD_VEC_NAME(std_string_push_str) (StdString *s, const char *str, size_t len)
{
	STD_VEC_NAME(std_vec_extend_from_slice) (s, str, len, 1, 1);
}

/* std_string_from is String::from of a str, which allocates its length exactly. */
void
STD_VEC_NAME(std_string_from) (StdString *s, const char *str, size_t len)
{
	STD_VEC_NAME(std_vec_with_capacity) (s, len, 1, 1);
	if (len > 0)
	{
		STD_VEC_CAT(std_ptr_copy_nonoverlapping_, STD_VEC_SUFFIX) (str, s->ptr, len, 1, 1);
		STD_VEC_NAME(std_vec_set_len) (s, len);
	}
}

/*
 * std_slice_to_vec is <[T]>::to_vec of a Copy T, ConvertVec's: a vector of
 * the slice's length, its elements copied in by copy_to_nonoverlapping.
 */
void
STD_VEC_NAME(std_slice_to_vec) (StdVec *v, const void *s, size_t len, size_t size, size_t align)
{
	STD_VEC_NAME(std_vec_with_capacity) (v, len, size, align);
	STD_VEC_CAT(std_ptr_copy_nonoverlapping_, STD_VEC_SUFFIX) (s, v->ptr, len, size, align);
	STD_VEC_NAME(std_vec_set_len) (v, len);
}

/*
 * std_box_slice_into_vec is <[T]>::into_vec of a box of n elements, vec!'s
 * last step: from_raw_parts_in of its block, the capacity the length, a
 * zero-sized type's room usize::MAX.
 */
void
STD_VEC_NAME(std_box_slice_into_vec) (StdVec *v, void *ptr, size_t n, size_t size)
{
#if STD_VEC_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	size_t		capacity = n;

	if (!(n <= capacity))
		STD_PTR_VIOLATED("Vec::from_raw_parts_in requires that length <= capacity", VEC_FILE, 1191, 9);
#endif
#if STD_VEC_UB_CHECKS
	std_NonNull_new_unchecked_check(ptr, STD_PTR_UNIQUE_FILE, 88, 36);
#endif
	v->ptr = ptr;
	v->len = n;
	v->cap = size == 0 ? SIZE_MAX : n;
}

#undef STD_VEC_RETAIN_FRAME_0_T

#endif							/* STD_VEC_DEFINE */

#undef STD_VEC_CAT_
#undef STD_VEC_CAT
#undef STD_VEC_NAME
#undef STD_VEC_DECLARE
#undef STD_VEC_DEFINE
#undef STD_VEC_UB_CHECKS
#undef STD_VEC_SUFFIX
