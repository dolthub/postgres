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
 * The template of core's ptr::copy_nonoverlapping, <[T]>::copy_from_slice, and
 * <*const T>::offset_from_unsigned,
 * whose precondition checks run where they are codegened (the port text's
 * section 3), ported from core/src/ptr/mod.rs and core/src/slice/mod.rs at
 * Rust 1.99.1. A crate's copy is codegened in the crate, checked in the
 * cassert build; core's and std's own non-inline code's is not. The
 * including file defines PTR_FILE, std_ub_checks_maybe_is_aligned_and_not_null,
 * and std_ub_checks_maybe_is_nonoverlapping for the checked instance.
 * STD_COPY_DECLARE and STD_COPY_DEFINE choose the declarations and
 * definitions, STD_COPY_UB_CHECKS is 1 for the checked instance and 0 for the
 * other, and STD_COPY_SUFFIX names the instance; every parameter is undefined
 * at the end.
 */

/* STD_COPY_CAT pastes two tokens after expanding them. */
#define STD_COPY_CAT_(a, b) a##b
#define STD_COPY_CAT(a, b) STD_COPY_CAT_(a, b)

/* STD_COPY_NAME names the instance's functions. */
#define STD_COPY_NAME(name) STD_COPY_CAT(name##_, STD_COPY_SUFFIX)

#ifdef STD_COPY_DECLARE
extern void STD_COPY_NAME(std_ptr_copy_nonoverlapping) (const void *src, void *dst, size_t count, size_t size,
														size_t align);
extern void STD_COPY_NAME(std_slice_copy_from_slice) (void *dst, size_t dst_len, const void *src, size_t src_len,
													  size_t size, size_t align, const char *file, uint32 line,
													  uint32 col);
extern size_t STD_COPY_NAME(std_ptr_offset_from_unsigned) (const void *self, const void *origin, size_t size,
														   const char *file, uint32 line, uint32 col);
#endif

#ifdef STD_COPY_DEFINE

/*
 * std_ptr_copy_nonoverlapping is ptr::copy_nonoverlapping of count elements
 * of a size and alignment, its precondition checked at core's own location.
 */
void
STD_COPY_NAME(std_ptr_copy_nonoverlapping) (const void *src, void *dst, size_t count, size_t size, size_t align)
{
#if STD_COPY_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	StdUnwind	unwind;
	bool		zero_size = count == 0 || size == 0;
	bool		valid;

	STD_NOUNWIND_PUSH(&unwind);
	valid = std_ub_checks_maybe_is_aligned_and_not_null(src, align, zero_size) &&
		std_ub_checks_maybe_is_aligned_and_not_null(dst, align, zero_size) &&
		std_ub_checks_maybe_is_nonoverlapping(src, dst, size, count);
	std_unwind_pop(&unwind);
	if (!valid)
		STD_PTR_VIOLATED("ptr::copy_nonoverlapping requires that both pointer arguments are aligned and non-null "
						 "and the specified memory ranges do not overlap", PTR_FILE, 553, 5);
#endif
	memcpy(dst, src, count * size);
}

/*
 * std_slice_copy_from_slice is <[T]>::copy_from_slice: the source's
 * elements copied over the destination's by ptr::copy_nonoverlapping,
 * panicking at its caller, as len_mismatch_fail does, for slices of
 * different lengths.
 */
void
STD_COPY_NAME(std_slice_copy_from_slice) (void *dst, size_t dst_len, const void *src, size_t src_len, size_t size,
										  size_t align, const char *file, uint32 line, uint32 col)
{
	if (dst_len != src_len)
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("copy_from_slice: source slice length ("), STD_FMT_ARG(0),
			STD_FMT_LIT(") does not match destination slice length ("), STD_FMT_ARG(1), STD_FMT_LIT(")"),
		};
		StdFmtArg	args[2];
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		args[0].value = &src_len;
		args[0].fmt = std_usize_Display_fmt;
		args[1].value = &dst_len;
		args[1].fmt = std_usize_Display_fmt;
		std_panic_fmt(file, line, col, &arguments);
	}
	STD_COPY_NAME(std_ptr_copy_nonoverlapping) (src, dst, dst_len, size, align);
}

/*
 * std_ptr_offset_from_unsigned is <*const T>::offset_from_unsigned, which is
 * #[track_caller]: its distance from an origin no later than it, in elements
 * of a size, the precondition at its caller; its assertion of the pointee's
 * size, a constant, shipped core's MIR proves away.
 */
size_t
STD_COPY_NAME(std_ptr_offset_from_unsigned) (const void *self, const void *origin, size_t size, const char *file,
											 uint32 line, uint32 col)
{
#if STD_COPY_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	if (!((uintptr_t) self >= (uintptr_t) origin))
		STD_PTR_VIOLATED("ptr::offset_from_unsigned requires `self >= origin`", file, line, col);
#endif
	return ((uintptr_t) self - (uintptr_t) origin) / size;
}

#endif							/* STD_COPY_DEFINE */

#undef STD_COPY_CAT_
#undef STD_COPY_CAT
#undef STD_COPY_NAME
#undef STD_COPY_DECLARE
#undef STD_COPY_DEFINE
#undef STD_COPY_UB_CHECKS
#undef STD_COPY_SUFFIX
