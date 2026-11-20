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
 * The C forms of core's unsafe pointer and slice operations whose
 * assert_unsafe_precondition! checks shipped core keeps, ported from
 * core/src/ptr, core/src/slice, core/src/hint.rs, core/src/alloc/layout.rs,
 * and core/src/ub_checks.rs at Rust 1.99.1 (the port text's section 3).
 * Each check is precondition_check's: the precondition's own test under
 * USE_ASSERT_CHECKING and, on a violation, core's whole message through the
 * panic hook at the check's location, then an abort without unwinding. A
 * check of a #[track_caller] function is at its caller's location, and
 * another's at the macro's own line in core.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "port/pg_bitutils.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#define PTR_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "ptr" STD_SEP "mod.rs"
#define CONST_PTR_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "ptr" STD_SEP "const_ptr.rs"
#define UB_CHECKS_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "ub_checks.rs"
#define HINT_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "hint.rs"
#define ALIGNMENT_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "mem" STD_SEP "alignment.rs"
#define LAYOUT_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "alloc" STD_SEP "layout.rs"

/* PANICKING_FILE is core/src/panicking.rs, where panic_nounwind, which takes no caller's location, panics. */
#define PANICKING_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "panicking.rs"

#ifdef USE_ASSERT_CHECKING

/* std_ptr_is_aligned_to is <*const T>::is_aligned_to: the address a multiple of a power-of-two alignment. */
static bool
std_ptr_is_aligned_to(const void *self, size_t align)
{
	if (!(align != 0 && (align & (align - 1)) == 0))
	{
		static const char text[] = "is_aligned_to: align is not a power-of-two";

		std_panic(CONST_PTR_FILE, 1357, 13, text, sizeof(text) - 1);
	}
	return ((uintptr_t) self & (align - 1)) == 0;
}

/* std_ub_checks_maybe_is_aligned is ub_checks' maybe_is_aligned: at run time, the pointer's is_aligned_to. */
static bool
std_ub_checks_maybe_is_aligned(const void *ptr, size_t align)
{
	return std_ptr_is_aligned_to(ptr, align);
}

/* std_ub_checks_maybe_is_aligned_and_not_null is ub_checks' maybe_is_aligned_and_not_null. */
static bool
std_ub_checks_maybe_is_aligned_and_not_null(const void *ptr, size_t align, bool is_zst)
{
	return std_ub_checks_maybe_is_aligned(ptr, align) && (is_zst || ptr != NULL);
}

/*
 * std_Alignment_new_unchecked_check is Alignment::new_unchecked's check,
 * which is #[track_caller]: a power of two.
 */
static void
std_Alignment_new_unchecked_check(size_t align, const char *file, uint32 line, uint32 col)
{
	if (!(align != 0 && (align & (align - 1)) == 0))
		STD_PTR_VIOLATED("Alignment::new_unchecked requires a power of two", file, line, col);
}

/* std_Alignment_new is Alignment::new: whether the alignment is a power of two, made by new_unchecked when it is. */
static bool
std_Alignment_new(size_t align)
{
	if (align != 0 && (align & (align - 1)) == 0)
	{
		std_Alignment_new_unchecked_check(align, ALIGNMENT_FILE, 173, 27);
		return true;
	}
	return false;
}

/*
 * std_Layout_is_size_align_valid is Layout::is_size_align_valid: a
 * power-of-two alignment and a size within max_size_for_alignment, the
 * largest whose round-up stays within isize::MAX.
 */
static bool
std_Layout_is_size_align_valid(size_t size, size_t align)
{
	if (!std_Alignment_new(align))
		return false;
	return size <= ((size_t) PG_INT64_MAX + 1) - align;
}

/* std_ub_checks_is_valid_allocation_size is ub_checks' is_valid_allocation_size. */
static bool
std_ub_checks_is_valid_allocation_size(size_t size, size_t len)
{
	size_t		max_len = size == 0 ? SIZE_MAX : (size_t) PG_INT64_MAX / size;

	return len <= max_len;
}

/*
 * std_ub_checks_maybe_is_nonoverlapping is ub_checks' maybe_is_nonoverlapping:
 * whether two ranges of count elements of a size are disjoint, a size whose
 * product overflows aborting as panic_nounwind does.
 */
static bool
std_ub_checks_maybe_is_nonoverlapping(const void *src, const void *dst, size_t size, size_t count)
{
	uintptr_t	src_usize = (uintptr_t) src;
	uintptr_t	dst_usize = (uintptr_t) dst;
	size_t		total;
	uintptr_t	diff;

	if (!std_checked_mul_usize(size, count, &total))
	{
		static const char text[] = "is_nonoverlapping: `size_of::<T>() * count` overflows a usize";

		std_panic_nounwind(PANICKING_FILE, 225, 5, text, sizeof(text) - 1);
	}
	diff = src_usize > dst_usize ? src_usize - dst_usize : dst_usize - src_usize;
	return diff >= total;
}

#endif							/* USE_ASSERT_CHECKING */

/* copy_nonoverlapping and copy_from_slice as the crates' code runs them, checked, and as std's own code does. */
#define STD_COPY_DEFINE
#define STD_COPY_UB_CHECKS 1
#define STD_COPY_SUFFIX Checked
#include "doltgres/std/ptr_copy_template.h"
#define STD_COPY_DEFINE
#define STD_COPY_UB_CHECKS 0
#define STD_COPY_SUFFIX Unchecked
#include "doltgres/std/ptr_copy_template.h"

/*
 * std_ptr_align_offset_mod_inv is align_offset's mod_inv: the inverse of x
 * modulo a power of two m, from INV_TABLE_MOD_16 by Newton's iteration.
 */
static size_t
std_ptr_align_offset_mod_inv(size_t x, size_t m)
{
	static const uint8 INV_TABLE_MOD_16[8] = {1, 11, 13, 7, 9, 3, 5, 15};
	const size_t INV_TABLE_MOD = 16;
	size_t		m_minus_one = m - 1;
	size_t		index = (x & (INV_TABLE_MOD - 1)) >> 1;
	size_t		inverse;
	size_t		mod_gate = INV_TABLE_MOD;

	std_index_check(index, lengthof(INV_TABLE_MOD_16), PTR_FILE, 2316, 27);
	inverse = INV_TABLE_MOD_16[index];
	for (;;)
	{
		size_t		new_gate;

		if (mod_gate >= m)
			break;
		inverse = inverse * (2 - x * inverse);
		if (!std_checked_mul_usize(mod_gate, mod_gate, &new_gate))
			break;
		mod_gate = new_gate;
	}
	return inverse & m_minus_one;
}

/*
 * std_ptr_align_offset is ptr::align_offset: the elements of a stride to add
 * to a pointer to align it to a power of two a, or usize::MAX when no count
 * of elements can.
 */
size_t
std_ptr_align_offset(const void *p, size_t stride, size_t a)
{
	size_t		addr = (uintptr_t) p;
	size_t		a_minus_one = a - 1;
	size_t		a_mod_stride;
	uint32		gcdpow;
	size_t		gcd;

	if (stride == 0)
	{
		size_t		p_mod_a = addr & a_minus_one;

		return p_mod_a == 0 ? 0 : SIZE_MAX;
	}
	a_mod_stride = a % stride;
	if (a_mod_stride == 0)
	{
		size_t		aligned_address = (addr + a_minus_one) & (0 - a);
		size_t		byte_offset = aligned_address - addr;
		size_t		addr_mod_stride = addr % stride;

		return addr_mod_stride == 0 ? byte_offset / stride : SIZE_MAX;
	}
	gcdpow = Min(pg_rightmost_one_pos64(stride), pg_rightmost_one_pos64(a));
	gcd = (size_t) 1 << gcdpow;
	if ((addr & (gcd - 1)) == 0)
	{
		size_t		a2 = a >> gcdpow;
		size_t		a2minus1 = a2 - 1;
		size_t		s2 = (stride & a_minus_one) >> gcdpow;
		size_t		minusp2 = a2 - ((addr & a_minus_one) >> gcdpow);

		return (minusp2 * std_ptr_align_offset_mod_inv(s2, a2)) & a2minus1;
	}
	return SIZE_MAX;
}

/* std_ptr_const_align_offset is <*const T>::align_offset: ptr::align_offset after its power-of-two check. */
size_t
std_ptr_const_align_offset(const void *self, size_t stride, size_t align)
{
	if (!(align != 0 && (align & (align - 1)) == 0))
	{
		static const char text[] = "align_offset: align is not a power-of-two";

		std_panic(CONST_PTR_FILE, 1282, 13, text, sizeof(text) - 1);
	}
	return std_ptr_align_offset(self, stride, align);
}

/* std_ptr_copy is ptr::copy of count elements of a size and alignment, the ranges possibly overlapping. */
void
std_ptr_copy(const void *src, void *dst, size_t count, size_t size, size_t align)
{
#ifdef USE_ASSERT_CHECKING
	StdUnwind	unwind;
	bool		zero_size = size == 0 || count == 0;
	bool		valid;

	STD_NOUNWIND_PUSH(&unwind);
	valid = std_ub_checks_maybe_is_aligned_and_not_null(src, align, zero_size) &&
		std_ub_checks_maybe_is_aligned_and_not_null(dst, align, zero_size);
	std_unwind_pop(&unwind);
	if (!valid)
		STD_PTR_VIOLATED("ptr::copy requires that both pointer arguments are aligned and non-null", PTR_FILE, 652, 9);
#endif
	memmove(dst, src, count * size);
}

/* std_ptr_write_bytes is ptr::write_bytes: count elements of a size and alignment set to a byte. */
void
std_ptr_write_bytes(void *dst, uint8 val, size_t count, size_t size, size_t align)
{
#ifdef USE_ASSERT_CHECKING
	StdUnwind	unwind;
	bool		zero_size = size == 0 || count == 0;
	bool		valid;

	STD_NOUNWIND_PUSH(&unwind);
	valid = std_ub_checks_maybe_is_aligned_and_not_null(dst, align, zero_size);
	std_unwind_pop(&unwind);
	if (!valid)
		STD_PTR_VIOLATED("ptr::write_bytes requires that the destination pointer is aligned and non-null",
						 PTR_FILE, 726, 9);
#endif
	memset(dst, val, count * size);
}

/* std_ptr_swap_chunk is swap_chunk: two chunks of n bytes exchanged through copies of both. */
static void
std_ptr_swap_chunk(uint8 *x, uint8 *y, size_t n)
{
	uint8		a[8];
	uint8		b[8];

	memcpy(a, x, n);
	memcpy(b, y, n);
	memcpy(x, b, n);
	memcpy(y, a, n);
}

/*
 * std_ptr_swap_nonoverlapping_bytes is swap_nonoverlapping_bytes: the bytes
 * swapped a pointer-sized chunk at a time, then the tail by 4, 2, and 1 as
 * its bits say.
 */
static void
std_ptr_swap_nonoverlapping_bytes(uint8 *x, uint8 *y, size_t bytes)
{
	size_t		chunk_size = sizeof(void *);
	size_t		chunks = bytes / chunk_size;
	size_t		tail = bytes % chunk_size;

	for (size_t i = 0; i < chunks; i++)
		std_ptr_swap_chunk(x + i * chunk_size, y + i * chunk_size, chunk_size);
	if (tail != 0)
	{
		size_t		delta = chunks * chunk_size;
		size_t		i = 0;

		if ((tail & 4) != 0)
		{
			std_ptr_swap_chunk(x + delta + i, y + delta + i, 4);
			i |= 4;
		}
		if ((tail & 2) != 0)
		{
			std_ptr_swap_chunk(x + delta + i, y + delta + i, 2);
			i |= 2;
		}
		if ((tail & 1) != 0)
		{
			std_ptr_swap_chunk(x + delta + i, y + delta + i, 1);
			i |= 1;
		}
	}
}

/*
 * std_ptr_swap_nonoverlapping is ptr::swap_nonoverlapping of count elements
 * of a size and alignment, #[track_caller], so its check is at its caller.
 */
void
std_ptr_swap_nonoverlapping(void *x, void *y, size_t count, size_t size, size_t align, const char *file,
							uint32 line, uint32 col)
{
	size_t		bytes;

#ifdef USE_ASSERT_CHECKING
	StdUnwind	unwind;
	bool		zero_size = size == 0 || count == 0;
	bool		valid;

	STD_NOUNWIND_PUSH(&unwind);
	valid = std_ub_checks_maybe_is_aligned_and_not_null(x, align, zero_size) &&
		std_ub_checks_maybe_is_aligned_and_not_null(y, align, zero_size) &&
		std_ub_checks_maybe_is_nonoverlapping(x, y, size, count);
	std_unwind_pop(&unwind);
	if (!valid)
		STD_PTR_VIOLATED("ptr::swap_nonoverlapping requires that both pointer arguments are aligned and non-null "
						 "and the specified memory ranges do not overlap", file, line, col);
#endif
	bytes = count * size;
	if (bytes != 0)
		std_ptr_swap_nonoverlapping_bytes((uint8 *) x, (uint8 *) y, bytes);
}

/*
 * std_slice_from_raw_parts_check is slice::from_raw_parts' check, which is
 * #[track_caller]: an aligned, non-null pointer and a slice of at most
 * isize::MAX bytes.
 */
void
std_slice_from_raw_parts_check(const void *data, size_t len, size_t size, size_t align, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	StdUnwind	unwind;
	bool		valid;

	STD_NOUNWIND_PUSH(&unwind);
	valid = std_ub_checks_maybe_is_aligned_and_not_null(data, align, false) &&
		std_ub_checks_is_valid_allocation_size(size, len);
	std_unwind_pop(&unwind);
	if (!valid)
		STD_PTR_VIOLATED("slice::from_raw_parts requires the pointer to be aligned and non-null, and the total size "
						 "of the slice not to exceed `isize::MAX`", file, line, col);
#endif
}

/* std_slice_from_raw_parts_mut_check is slice::from_raw_parts_mut's check, as from_raw_parts'. */
void
std_slice_from_raw_parts_mut_check(const void *data, size_t len, size_t size, size_t align, const char *file,
								   uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	StdUnwind	unwind;
	bool		valid;

	STD_NOUNWIND_PUSH(&unwind);
	valid = std_ub_checks_maybe_is_aligned_and_not_null(data, align, false) &&
		std_ub_checks_is_valid_allocation_size(size, len);
	std_unwind_pop(&unwind);
	if (!valid)
		STD_PTR_VIOLATED("slice::from_raw_parts_mut requires the pointer to be aligned and non-null, and the total "
						 "size of the slice not to exceed `isize::MAX`", file, line, col);
#endif
}

/*
 * std_hint_unreachable_unchecked is hint::unreachable_unchecked, which is
 * #[track_caller]: its check always fails; outside USE_ASSERT_CHECKING
 * reaching it is undefined, as Rust's is.
 */
void
std_hint_unreachable_unchecked(const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	STD_PTR_VIOLATED("hint::unreachable_unchecked must never be reached", file, line, col);
#endif
	pg_unreachable();
}

/* std_NonNull_new_unchecked_check is NonNull::new_unchecked's check, which is #[track_caller]: a non-null pointer. */
void
std_NonNull_new_unchecked_check(const void *ptr, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (ptr == NULL)
		STD_PTR_VIOLATED("NonNull::new_unchecked requires that the pointer is non-null", file, line, col);
#endif
}

/*
 * std_Layout_from_size_align_unchecked_check is Layout::from_size_align_unchecked's
 * check, which is #[track_caller]: a power-of-two alignment and a size that
 * rounded up to it stays within isize::MAX.
 */
void
std_Layout_from_size_align_unchecked_check(size_t size, size_t align, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!std_Layout_is_size_align_valid(size, align))
		STD_PTR_VIOLATED("Layout::from_size_align_unchecked requires that align is a power of 2 "
						 "and the rounded-up allocation size does not exceed isize::MAX", file, line, col);
#endif
}

/*
 * std_Layout_for_value_raw_check is Layout::for_value_raw's checks of a
 * value's size and alignment: Alignment::of_val_raw's new_unchecked, then
 * from_size_alignment_unchecked's size within max_size_for_alignment.
 */
void
std_Layout_for_value_raw_check(size_t size, size_t align)
{
#ifdef USE_ASSERT_CHECKING
	std_Alignment_new_unchecked_check(align, ALIGNMENT_FILE, 161, 18);
	if (!(size <= ((size_t) PG_INT64_MAX + 1) - align))
		STD_PTR_VIOLATED("Layout::from_size_alignment_unchecked requires that the rounded-up allocation size does not "
						 "exceed isize::MAX", LAYOUT_FILE, 263, 18);
#endif
}

/*
 * std_slice_get_unchecked_check is slice::get_unchecked's check of an
 * index, which is #[track_caller]: the index within the slice.
 */
void
std_slice_get_unchecked_check(size_t index, size_t len, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!(index < len))
		STD_PTR_VIOLATED("slice::get_unchecked requires that the index is within the slice", file, line, col);
#endif
}

/*
 * std_slice_get_unchecked_mut_check is slice::get_unchecked_mut's check of
 * an index, which is #[track_caller]: the index within the slice.
 */
void
std_slice_get_unchecked_mut_check(size_t index, size_t len, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!(index < len))
		STD_PTR_VIOLATED("slice::get_unchecked_mut requires that the index is within the slice", file, line, col);
#endif
}

/*
 * std_usize_unchecked_add_check is usize::unchecked_add's check, which is
 * #[track_caller]: a sum that doesn't overflow.
 */
void
std_usize_unchecked_add_check(size_t lhs, size_t rhs, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (lhs + rhs < lhs)
		STD_PTR_VIOLATED("usize::unchecked_add cannot overflow", file, line, col);
#endif
}

/*
 * std_usize_unchecked_sub_check is usize::unchecked_sub's check, which is
 * #[track_caller]: the difference not below zero.
 */
void
std_usize_unchecked_sub_check(size_t lhs, size_t rhs, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (lhs < rhs)
		STD_PTR_VIOLATED("usize::unchecked_sub cannot overflow", file, line, col);
#endif
}

/*
 * std_usize_unchecked_mul_check is usize::unchecked_mul's check, which is
 * #[track_caller]: the product within usize.
 */
void
std_usize_unchecked_mul_check(size_t lhs, size_t rhs, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	size_t		product;

	if (!std_checked_mul_usize(lhs, rhs, &product))
		STD_PTR_VIOLATED("usize::unchecked_mul cannot overflow", file, line, col);
#endif
}

/*
 * std_u8_unchecked_add_check is u8::unchecked_add's check, which is
 * #[track_caller]: the sum within u8.
 */
void
std_u8_unchecked_add_check(uint8 lhs, uint8 rhs, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if ((uint8) (lhs + rhs) < lhs)
		STD_PTR_VIOLATED("u8::unchecked_add cannot overflow", file, line, col);
#endif
}

/*
 * std_u32_unchecked_add_check is u32::unchecked_add's check, which is
 * #[track_caller]: the sum within u32.
 */
void
std_u32_unchecked_add_check(uint32 lhs, uint32 rhs, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if ((uint32) (lhs + rhs) < lhs)
		STD_PTR_VIOLATED("u32::unchecked_add cannot overflow", file, line, col);
#endif
}

/*
 * std_u64_unchecked_add_check is u64::unchecked_add's check, which is
 * #[track_caller]: the sum within u64.
 */
void
std_u64_unchecked_add_check(uint64 lhs, uint64 rhs, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if ((uint64) (lhs + rhs) < lhs)
		STD_PTR_VIOLATED("u64::unchecked_add cannot overflow", file, line, col);
#endif
}

/*
 * std_Layout_from_size_align is Layout::from_size_align: the layout when
 * is_size_align_valid holds, else LayoutError.
 */
bool
std_Layout_from_size_align(size_t size, size_t align, StdLayout *result, StdLayoutError *error)
{
	if (std_Layout_is_size_align_valid(size, align))
	{
		result->size = size;
		result->align = align;
		return true;
	}
	return false;
}

/* std_LayoutError_Debug_fmt is LayoutError's derived Debug: its name. */
bool
std_LayoutError_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_write_str(f, "LayoutError", 11);
}

/*
 * std_mem_transmute_copy_check is mem::transmute_copy's check, which is
 * #[track_caller]: the destination's size no larger than the source's.
 */
void
std_mem_transmute_copy_check(size_t src_size, size_t dst_size, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!(src_size >= dst_size))
		STD_PTR_VIOLATED("cannot transmute_copy if Dst is larger than Src", file, line, col);
#endif
}

/*
 * std_slice_get_unchecked_range_check is slice::get_unchecked's check of a
 * range, which is #[track_caller]: the range ordered and within the slice.
 */
void
std_slice_get_unchecked_range_check(size_t start, size_t end, size_t len, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!(end >= start && end <= len))
		STD_PTR_VIOLATED("slice::get_unchecked requires that the range is within the slice", file, line, col);
#endif
}

/*
 * std_slice_get_unchecked_mut_range_check is slice::get_unchecked_mut's
 * check of a range, which is #[track_caller]: the range ordered and within
 * the slice.
 */
void
std_slice_get_unchecked_mut_range_check(size_t start, size_t end, size_t len, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!(end >= start && end <= len))
		STD_PTR_VIOLATED("slice::get_unchecked_mut requires that the range is within the slice", file, line, col);
#endif
}

/* std_NonZero_new_unchecked_check is NonZero::new_unchecked's check, which is #[track_caller]: a nonzero value. */
void
std_NonZero_new_unchecked_check(size_t n, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (n == 0)
		STD_PTR_VIOLATED("NonZero::new_unchecked requires the argument to be non-zero", file, line, col);
#endif
}

/*
 * std_hint_assert_unchecked is hint::assert_unchecked, which is
 * #[track_caller]: a condition the caller promises holds.
 */
void
std_hint_assert_unchecked(bool cond, const char *file, uint32 line, uint32 col)
{
#ifdef USE_ASSERT_CHECKING
	if (!cond)
		STD_PTR_VIOLATED("hint::assert_unchecked must never be called when the condition is false", file, line, col);
#endif
}

/*
 * std_check_aligned is CheckAlignment's check: a pointer that isn't a
 * multiple of its pointee's alignment reports core's
 * panic_misaligned_pointer_dereference, which can't unwind.
 */
void
std_check_aligned(const void *ptr, size_t align, const char *file, uint32 line, uint32 col)
{
	if (((uintptr_t) ptr & (align - 1)) != 0)
	{
		char text[	128];
		int			len = snprintf(text, sizeof(text),
								   "misaligned pointer dereference: address must be a multiple of 0x%zx but is 0x%zx",
								   align, (size_t) (uintptr_t) ptr);

		std_panic_nounwind(file, line, col, text, (size_t) len);
	}
}

/* std_check_null_deref is CheckNull's check of a read or write: core's panic_null_pointer_dereference. */
void
std_check_null_deref(const void *ptr, const char *file, uint32 line, uint32 col)
{
	static const char text[] = "null pointer dereference occurred";

	if (ptr == NULL)
		std_panic_nounwind(file, line, col, text, sizeof(text) - 1);
}

/* std_check_null_ref is CheckNull's check of a reference made from a pointer: core's panic_null_reference_constructed. */
void
std_check_null_ref(const void *ptr, const char *file, uint32 line, uint32 col)
{
	static const char text[] = "null reference produced";

	if (ptr == NULL)
		std_panic_nounwind(file, line, col, text, sizeof(text) - 1);
}
