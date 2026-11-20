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
 * assert_unsafe_precondition! checks shipped core keeps (the port text's
 * section 3): each runs the precondition's own test under
 * USE_ASSERT_CHECKING, the build Rust's ub_checks() is on in, and a
 * violation reports core's whole message at the check's location and
 * aborts without unwinding. A std form that reaches one of these from a
 * crate calls it; std's own non-inline code, which runs no checks, does the
 * operation itself.
 */
#ifndef DOLTGRES_STD_PTR_H
#define DOLTGRES_STD_PTR_H

#include "doltgres/std/unwind.h"

/* STD_PTR_UNIQUE_FILE is core's ptr/unique.rs, where Unique's calls report. */
#define STD_PTR_UNIQUE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "ptr" STD_SEP "unique.rs"

/* STD_ARRAY_MOD_FILE and STD_ARRAY_DRAIN_FILE are core's array/mod.rs and array/drain.rs, where map's checks report. */
#define STD_ARRAY_MOD_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "array" STD_SEP "mod.rs"
#define STD_ARRAY_DRAIN_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "array" STD_SEP "drain.rs"

/* STD_PTR_NON_NULL_FILE is core's ptr/non_null.rs, where NonNull's calls report. */
#define STD_PTR_NON_NULL_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "ptr" STD_SEP "non_null.rs"

/* STD_PTR_UB_SUFFIX is the text precondition_check puts after every precondition's message. */
#define STD_PTR_UB_SUFFIX "\n\nThis indicates a bug in the program. " \
	"This Undefined Behavior check is optional, and cannot be relied on for safety."

/*
 * STD_PTR_VIOLATED reports a violated precondition of core's message, a
 * literal, at a location and aborts without unwinding.
 */
#define STD_PTR_VIOLATED(message, file, line, col) \
	std_panic_nounwind((file), (line), (col), "unsafe precondition(s) violated: " message STD_PTR_UB_SUFFIX, \
					   sizeof("unsafe precondition(s) violated: " message STD_PTR_UB_SUFFIX) - 1)

extern size_t std_ptr_align_offset(const void *p, size_t stride, size_t a);
extern size_t std_ptr_const_align_offset(const void *self, size_t stride, size_t align);
#define STD_COPY_DECLARE
#define STD_COPY_SUFFIX Checked
#include "doltgres/std/ptr_copy_template.h"
#define STD_COPY_DECLARE
#define STD_COPY_SUFFIX Unchecked
#include "doltgres/std/ptr_copy_template.h"

/*
 * STD_PTR_AT is ptr::add of i elements of a size to p, the address of the
 * element i places on; its precondition is under core's own
 * cfg(debug_assertions), which shipped core is built without, so it has no
 * check.
 */
#define STD_PTR_AT(p, i, size) ((void *) ((char *) (p) + (i) * (size)))

/* StdLayout is alloc::Layout: a size and its power-of-two alignment. */
typedef struct StdLayout
{
	size_t		size;
	size_t		align;
} StdLayout;

/* StdLayoutError is LayoutError, a unit struct. */
typedef struct StdLayoutError
{
	char		unused;
} StdLayoutError;

struct StdFormatter;
extern void std_ptr_copy(const void *src, void *dst, size_t count, size_t size, size_t align);
extern void std_ptr_write_bytes(void *dst, uint8 val, size_t count, size_t size, size_t align);
extern void std_ptr_swap_nonoverlapping(void *x, void *y, size_t count, size_t size, size_t align,
										const char *file, uint32 line, uint32 col);
extern void std_slice_from_raw_parts_check(const void *data, size_t len, size_t size, size_t align,
										   const char *file, uint32 line, uint32 col);
extern void std_slice_from_raw_parts_mut_check(const void *data, size_t len, size_t size, size_t align,
											   const char *file, uint32 line, uint32 col);
pg_noreturn extern void std_hint_unreachable_unchecked(const char *file, uint32 line, uint32 col);
extern void std_NonNull_new_unchecked_check(const void *ptr, const char *file, uint32 line, uint32 col);
extern void std_Layout_from_size_align_unchecked_check(size_t size, size_t align, const char *file, uint32 line, uint32 col);
extern void std_Layout_for_value_raw_check(size_t size, size_t align);
extern void std_usize_unchecked_add_check(size_t lhs, size_t rhs, const char *file, uint32 line, uint32 col);
extern void std_usize_unchecked_sub_check(size_t lhs, size_t rhs, const char *file, uint32 line, uint32 col);
extern void std_usize_unchecked_mul_check(size_t lhs, size_t rhs, const char *file, uint32 line, uint32 col);
extern void std_u8_unchecked_add_check(uint8 lhs, uint8 rhs, const char *file, uint32 line, uint32 col);
extern void std_u32_unchecked_add_check(uint32 lhs, uint32 rhs, const char *file, uint32 line, uint32 col);
extern void std_u64_unchecked_add_check(uint64 lhs, uint64 rhs, const char *file, uint32 line, uint32 col);
extern bool std_Layout_from_size_align(size_t size, size_t align, StdLayout *result, StdLayoutError *error);
extern bool std_LayoutError_Debug_fmt(const void *value, struct StdFormatter *f);
extern void std_mem_transmute_copy_check(size_t src_size, size_t dst_size, const char *file, uint32 line, uint32 col);
extern void std_slice_get_unchecked_check(size_t index, size_t len, const char *file, uint32 line, uint32 col);
extern void std_slice_get_unchecked_mut_check(size_t index, size_t len, const char *file, uint32 line, uint32 col);
extern void std_slice_get_unchecked_range_check(size_t start, size_t end, size_t len, const char *file,
												uint32 line, uint32 col);
extern void std_slice_get_unchecked_mut_range_check(size_t start, size_t end, size_t len, const char *file,
													uint32 line, uint32 col);
extern void std_NonZero_new_unchecked_check(size_t n, const char *file, uint32 line, uint32 col);
extern void std_hint_assert_unchecked(bool cond, const char *file, uint32 line, uint32 col);
extern void std_check_aligned(const void *ptr, size_t align, const char *file, uint32 line, uint32 col);
extern void std_check_null_deref(const void *ptr, const char *file, uint32 line, uint32 col);
extern void std_check_null_ref(const void *ptr, const char *file, uint32 line, uint32 col);

/*
 * The pointer checks rustc's MIR passes insert at a dereference in the
 * cassert build, where ub_checks are on: CheckAlignment's, then CheckNull's
 * (none for a zero-sized pointee), at the dereference's line and column.
 */
#ifdef USE_ASSERT_CHECKING
#define STD_CHECK_ALIGNED(ptr, align, line, col) std_check_aligned((ptr), (align), RUST_FILE, (line), (col))
#define STD_CHECK_NULL_DEREF(ptr, line, col) std_check_null_deref((ptr), RUST_FILE, (line), (col))
#define STD_CHECK_NULL_REF(ptr, line, col) std_check_null_ref((ptr), RUST_FILE, (line), (col))
#else
#define STD_CHECK_ALIGNED(ptr, align, line, col) ((void) 0)
#define STD_CHECK_NULL_DEREF(ptr, line, col) ((void) 0)
#define STD_CHECK_NULL_REF(ptr, line, col) ((void) 0)
#endif

#endif							/* DOLTGRES_STD_PTR_H */
