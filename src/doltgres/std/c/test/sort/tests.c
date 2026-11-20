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
 * The C port of core's sort tests' tests.rs (library/alloctests/tests/sort,
 * Rust 1.99.1), its stable instantiation: the stable sort, the known good
 * merge sort, and the tests' template at each element type the tests sort,
 * and the tests of define_instantiate_sort_tests!, each the test of its
 * name.
 */
#include "postgres.h"

#include "doltgres/std/alloc.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/rc.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_libm.h"
#include "doltgres/std/test_sort.h"

#define RUST_FILE STD_RUST_LIBRARY STD_SEP "alloctests" STD_SEP "tests" STD_SEP "sort" STD_SEP "tests.rs"

/* ALLOCTESTS_SORT_TESTS_COPY is a Copy type's Clone into a destination: its bytes. */
#define ALLOCTESTS_SORT_TESTS_COPY(dst, src) (*(dst) = *(src))

/* alloctests_sort_tests_stable_SortImpl_name is the stable instantiation's Sort::name, a String of its name. */
static void
alloctests_sort_tests_stable_SortImpl_name(StdString *result)
{
	static const char name[] = "rust_std_stable";

	std_slice_to_vec_Checked(result, name, sizeof(name) - 1, 1, 1);
}

/* AlloctestsSortTestsStableSortImplSortByClosure0 is SortImpl::sort_by's closure's captures: the comparison it calls, by reference. */
typedef struct AlloctestsSortTestsStableSortImplSortByClosure0
{
	StdOrdering (*compare) (void *captures, const void *a, const void *b);
	void	   *compare_captures;
} AlloctestsSortTestsStableSortImplSortByClosure0;

/* alloctests_sort_tests_stable_SortImpl_sort_by_closure_0 is SortImpl::sort_by's closure: compare(a, b). */
static StdOrdering
alloctests_sort_tests_stable_SortImpl_sort_by_closure_0(void *captures, const void *a, const void *b)
{
	AlloctestsSortTestsStableSortImplSortByClosure0 *closure = captures;

	return closure->compare(closure->compare_captures, a, b);
}

/*
 * ALLOCTESTS_SORT_TESTS_SORT_BY defines the stable instantiation's
 * Sort::sort_by at an element type: the slice's sort_by through a closure of
 * the comparison, which it takes and drops after.
 */
#define ALLOCTESTS_SORT_TESTS_SORT_BY(TS, T) \
static void \
alloctests_sort_tests_stable_SortImpl_sort_by_##TS(T *v, size_t len, \
												   StdOrdering (*compare) (void *captures, const void *a, const void *b), \
												   void *compare_captures, void (*compare_drop) (void *)) \
{ \
	AlloctestsSortTestsStableSortImplSortByClosure0 closure = {compare, compare_captures}; \
\
	std_slice_sort_by_##TS(v, len, alloctests_sort_tests_stable_SortImpl_sort_by_closure_0, &closure, NULL); \
	if (compare_drop != NULL) \
		compare_drop(compare_captures); \
}

/* AlloctestsSortTestsTupleI32I32 is an (i32, i32). */
typedef struct AlloctestsSortTestsTupleI32I32
{
	int32		f0;
	int32		f1;
} AlloctestsSortTestsTupleI32I32;

/* alloctests_sort_tests_TupleI32I32_PartialOrd_lt is (i32, i32)'s PartialOrd::lt, lexical_ord's chain, on pointers. */
static bool
alloctests_sort_tests_TupleI32I32_PartialOrd_lt(const AlloctestsSortTestsTupleI32I32 *a, const AlloctestsSortTestsTupleI32I32 *b)
{
	return a->f0 != b->f0 ? a->f0 < b->f0 : a->f1 < b->f1;
}

/* ALLOCTESTS_SORT_TESTS_I32_EQ is i32's PartialEq on pointers. */
#define ALLOCTESTS_SORT_TESTS_I32_EQ(a, b) (*(a) == *(b))

/*
 * alloctests_sort_tests_TupleI32I32_PartialOrd_lt_FnMut_call_mut is the fn
 * item TupleI32I32's PartialOrd::lt's FnMut shim, as the sort passes it for
 * is_less.
 */
static bool
alloctests_sort_tests_TupleI32I32_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return alloctests_sort_tests_TupleI32I32_PartialOrd_lt(a, b);
}

/* The stable sort and its shared parts at (i32, i32). */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS TupleI32I32
#define STD_SORT_T AlloctestsSortTestsTupleI32I32
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TupleI32I32
#define STD_STABLE_SORT_T AlloctestsSortTestsTupleI32I32
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT alloctests_sort_tests_TupleI32I32_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

/*
 * std_i32_PartialOrd_lt_FnMut_call_mut is the fn item i32's PartialOrd::lt's
 * FnMut shim, as the sort passes it for is_less.
 */
static bool
std_i32_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_i32_PartialOrd_lt(a, b);
}

/* The stable sort and its shared parts at i32. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS I32
#define STD_SORT_T int32
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS I32
#define STD_STABLE_SORT_T int32
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT std_i32_PartialOrd_lt_FnMut_call_mut
#define STD_STABLE_SORT_WITH_BY
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS I32
#define STD_PTR_T int32
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS I32
#define STD_SLICE_T int32
#include "doltgres/std/slice_template.h"

ALLOCTESTS_SORT_TESTS_SORT_BY(I32, int32)

#define STD_KG_TS I32
#define STD_KG_T int32
#define STD_KG_LT(a, b) std_i32_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS I32
#define STD_ST_CMP(a, b) std_i32_Ord_cmp((a), (b))
#define STD_ST_EQ ALLOCTESTS_SORT_TESTS_I32_EQ
#define STD_ST_PAIR_TS TupleI32I32
#define STD_ST_PAIR_T AlloctestsSortTestsTupleI32I32
#define STD_ST_T int32
#define STD_ST_LT(a, b) std_i32_PartialOrd_lt((a), (b))
#define STD_ST_CLONE ALLOCTESTS_SORT_TESTS_COPY
#define STD_ST_DEBUG std_i32_Debug_fmt
#define STD_ST_IN_PLACE 1
#include "doltgres/std/test_sort_tests_template.h"

/* alloctests_sort_tests_shift_i32_to_u32 is shift_i32_to_u32: the value moved into u32's range, keeping its order. */
static uint32
alloctests_sort_tests_shift_i32_to_u32(int32 val)
{
	return (uint32) STD_ADD(i64, (int64) val, (int64) PG_INT32_MAX + 1, 128, 6);
}

/* alloctests_sort_tests_extend_i32_to_u64 is extend_i32_to_u64: the value spread over u64's range, keeping its order. */
static uint64
alloctests_sort_tests_extend_i32_to_u64(int32 val)
{
	return STD_MUL(u64, (uint64) alloctests_sort_tests_shift_i32_to_u32(val), (uint64) PG_INT32_MAX, 138, 5);
}

/* alloctests_sort_tests_extend_i32_to_u128 is extend_i32_to_u128: the value spread over u128's range, keeping its order. */
static StdU128
alloctests_sort_tests_extend_i32_to_u128(int32 val)
{
	return std_mul_u128(std_u128_from_u64((uint64) alloctests_sort_tests_shift_i32_to_u32(val)),
						std_u128_from_u64((uint64) PG_INT64_MAX), RUST_FILE, 144, 5);
}

/* alloctests_sort_tests_string_from_i32 is string_from_i32: the shifted value as ten zero-padded digits, which sort as it does. */
static void
alloctests_sort_tests_string_from_i32(int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_extend_i32_to_u64_into is extend_i32_to_u64 as the Fn(i32) -> u64 a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_extend_i32_to_u64_into(void *captures, int32 val, uint64 *result)
{
	*result = alloctests_sort_tests_extend_i32_to_u64(val);
}

/* alloctests_sort_tests_extend_i32_to_u128_into is extend_i32_to_u128 as the Fn(i32) -> u128 a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_extend_i32_to_u128_into(void *captures, int32 val, StdU128 *result)
{
	*result = alloctests_sort_tests_extend_i32_to_u128(val);
}

/* alloctests_sort_tests_Cell_new_into is Cell::new as the Fn(i32) -> Cell<i32> a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_Cell_new_into(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_F128_new_into is F128::new as the Fn(i32) -> F128 a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_F128_new_into(void *captures, int32 val, AlloctestsSortFfiTypesF128 *result)
{
	*result = alloctests_sort_ffi_types_F128_new(val);
}

/* alloctests_sort_tests_FFIOneKibiByte_new_into is FFIOneKibiByte::new as the Fn(i32) -> FFIOneKibiByte a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_FFIOneKibiByte_new_into(void *captures, int32 val, AlloctestsSortFfiTypesFFIOneKibiByte *result)
{
	*result = alloctests_sort_ffi_types_FFIOneKibiByte_new(val);
}

/* alloctests_sort_tests_reverse_shift_i32_to_u32 is reverse_shift_i32_to_u32: shift_i32_to_u32's value moved back into i32's range. */
static int32
alloctests_sort_tests_reverse_shift_i32_to_u32(uint32 val)
{
	return (int32) STD_SUB(i64, (int64) val, (int64) PG_INT32_MAX + 1, 132, 6);
}

/* alloctests_sort_tests_i32_from_string is i32_from_string: the String's u32, unwrapped, moved back into i32's range. */
static int32
alloctests_sort_tests_i32_from_string(const StdString *val)
{
	uint32		parsed;
	StdParseIntError error;

	if (!std_str_parse_u32(val->ptr, val->len, &parsed, &error))
		STD_RESULT_EXPECT_FAILED(168, 49, "called `Result::unwrap()` on an `Err` value", &error,
								 std_ParseIntError_Debug_fmt);
	return alloctests_sort_tests_reverse_shift_i32_to_u32(parsed);
}

/* alloctests_sort_tests_i32_from_i32_into is i32_from_i32 as the Fn(i32) -> i32 a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_i32_from_i32_into(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_i32_from_i32_ref_fn is i32_from_i32_ref as the Fn(&i32) -> i32 a test takes, a fn item with no captures. */
static int32
alloctests_sort_tests_i32_from_i32_ref_fn(void *captures, const int32 *val)
{
	return *val;
}

/* alloctests_sort_tests_cell_i32_from_i32_into is cell_i32_from_i32 as the Fn(i32) -> Cell<i32> a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_cell_i32_from_i32_into(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_i32_from_cell_i32_fn is i32_from_cell_i32 as the Fn(&Cell<i32>) -> i32 a test takes, a fn item with no captures. */
static int32
alloctests_sort_tests_i32_from_cell_i32_fn(void *captures, const int32 *val)
{
	return *val;
}

/* alloctests_sort_tests_string_from_i32_into is string_from_i32 as the Fn(i32) -> String a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_string_from_i32_into(void *captures, int32 val, StdString *result)
{
	alloctests_sort_tests_string_from_i32(val, result);
}

/* alloctests_sort_tests_i32_from_string_fn is i32_from_string as the Fn(&String) -> i32 a test takes, a fn item with no captures. */
static int32
alloctests_sort_tests_i32_from_string_fn(void *captures, const StdString *val)
{
	return alloctests_sort_tests_i32_from_string(val);
}

/* ALLOCTESTS_SORT_TESTS_STRING_CLONE is String's Clone into a destination: its bytes copied. */
#define ALLOCTESTS_SORT_TESTS_STRING_CLONE(dst, src) std_vec_clone((dst), (src), 1, 1)

/*
 * std_u64_PartialOrd_lt_FnMut_call_mut is the fn item u64's PartialOrd::lt's
 * FnMut shim, as the sort passes it for is_less.
 */
static bool
std_u64_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_u64_PartialOrd_lt(a, b);
}

/* The stable sort, its shared parts, ptr and slice forms, the known good sort, and the tests' template at U64. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS U64
#define STD_SORT_T uint64
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS U64
#define STD_STABLE_SORT_T uint64
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT std_u64_PartialOrd_lt_FnMut_call_mut
#define STD_STABLE_SORT_WITH_BY
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS U64
#define STD_PTR_T uint64
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS U64
#define STD_SLICE_T uint64
#include "doltgres/std/slice_template.h"

ALLOCTESTS_SORT_TESTS_SORT_BY(U64, uint64)

#define STD_KG_TS U64
#define STD_KG_T uint64
#define STD_KG_LT(a, b) std_u64_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS U64
#define STD_ST_T uint64
#define STD_ST_LT(a, b) std_u64_PartialOrd_lt((a), (b))
#define STD_ST_CLONE ALLOCTESTS_SORT_TESTS_COPY
#define STD_ST_DEBUG std_u64_Debug_fmt
#define STD_ST_IN_PLACE 0
#include "doltgres/std/test_sort_tests_template.h"

/*
 * std_u128_PartialOrd_lt_FnMut_call_mut is the fn item u128's PartialOrd::lt's
 * FnMut shim, as the sort passes it for is_less.
 */
static bool
std_u128_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_u128_PartialOrd_lt(a, b);
}

/* The stable sort, its shared parts, ptr and slice forms, the known good sort, and the tests' template at U128. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS U128
#define STD_SORT_T StdU128
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS U128
#define STD_STABLE_SORT_T StdU128
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT std_u128_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS U128
#define STD_PTR_T StdU128
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS U128
#define STD_SLICE_T StdU128
#include "doltgres/std/slice_template.h"

#define STD_KG_TS U128
#define STD_KG_T StdU128
#define STD_KG_LT(a, b) std_u128_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS U128
#define STD_ST_T StdU128
#define STD_ST_LT(a, b) std_u128_PartialOrd_lt((a), (b))
#define STD_ST_CLONE ALLOCTESTS_SORT_TESTS_COPY
#define STD_ST_DEBUG std_u128_Debug_fmt
#define STD_ST_IN_PLACE 0
#include "doltgres/std/test_sort_tests_template.h"

/* AlloctestsSortTestsTupleCellI32I32 is a (Cell<i32>, i32). */
typedef struct AlloctestsSortTestsTupleCellI32I32
{
	int32		f0;
	int32		f1;
} AlloctestsSortTestsTupleCellI32I32;

/* alloctests_sort_tests_TupleCellI32I32_PartialOrd_lt is (Cell<i32>, i32)'s PartialOrd::lt, lexical_ord's chain, on pointers. */
static bool
alloctests_sort_tests_TupleCellI32I32_PartialOrd_lt(const AlloctestsSortTestsTupleCellI32I32 *a, const AlloctestsSortTestsTupleCellI32I32 *b)
{
	StdOrdering c = std_i32_Ord_cmp(&a->f0, &b->f0);

	return c != STD_ORDERING_EQUAL ? c == STD_ORDERING_LESS : a->f1 < b->f1;
}

/*
 * alloctests_sort_tests_TupleCellI32I32_PartialOrd_lt_FnMut_call_mut is the fn
 * item TupleCellI32I32's PartialOrd::lt's FnMut shim, as the sort passes it
 * for is_less.
 */
static bool
alloctests_sort_tests_TupleCellI32I32_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return alloctests_sort_tests_TupleCellI32I32_PartialOrd_lt(a, b);
}

/* The stable sort and its shared parts at (Cell<i32>, i32). */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS TupleCellI32I32
#define STD_SORT_T AlloctestsSortTestsTupleCellI32I32
#define STD_SORT_FREEZE 0
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TupleCellI32I32
#define STD_STABLE_SORT_T AlloctestsSortTestsTupleCellI32I32
#define STD_STABLE_SORT_FREEZE 0
#define STD_STABLE_SORT_LT alloctests_sort_tests_TupleCellI32I32_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

/* The stable sort, its shared parts, ptr and slice forms, the known good sort, and the tests' template at CellI32. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS CellI32
#define STD_SORT_T int32
#define STD_SORT_FREEZE 0
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS CellI32
#define STD_STABLE_SORT_T int32
#define STD_STABLE_SORT_FREEZE 0
#define STD_STABLE_SORT_LT std_i32_PartialOrd_lt_FnMut_call_mut
#define STD_STABLE_SORT_WITH_BY
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS CellI32
#define STD_PTR_T int32
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS CellI32
#define STD_SLICE_T int32
#include "doltgres/std/slice_template.h"

ALLOCTESTS_SORT_TESTS_SORT_BY(CellI32, int32)

#define STD_KG_TS CellI32
#define STD_KG_T int32
#define STD_KG_LT(a, b) std_i32_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS CellI32
#define STD_ST_CMP(a, b) std_i32_Ord_cmp((a), (b))
#define STD_ST_EQ ALLOCTESTS_SORT_TESTS_I32_EQ
#define STD_ST_PAIR_TS TupleCellI32I32
#define STD_ST_PAIR_T AlloctestsSortTestsTupleCellI32I32
#define STD_ST_T int32
#define STD_ST_LT(a, b) std_i32_PartialOrd_lt((a), (b))
#define STD_ST_CLONE ALLOCTESTS_SORT_TESTS_COPY
#define STD_ST_DEBUG std_CellI32_Debug_fmt
#define STD_ST_IN_PLACE 1
#include "doltgres/std/test_sort_tests_template.h"

/* alloctests_sort_tests_String_Ord_cmp is String's Ord, its bytes', on pointers. */
static StdOrdering
alloctests_sort_tests_String_Ord_cmp(const StdString *a, const StdString *b)
{
	return std_bytes_Ord_cmp(a->ptr, a->len, b->ptr, b->len);
}

/* alloctests_sort_tests_String_PartialEq_eq is String's PartialEq, its bytes', on pointers. */
static bool
alloctests_sort_tests_String_PartialEq_eq(const StdString *a, const StdString *b)
{
	return a->len == b->len && (a->len == 0 || memcmp(a->ptr, b->ptr, a->len) == 0);
}

/* AlloctestsSortTestsTupleStringI32 is a (String, i32). */
typedef struct AlloctestsSortTestsTupleStringI32
{
	StdString	f0;
	int32		f1;
} AlloctestsSortTestsTupleStringI32;

/* alloctests_sort_tests_TupleStringI32_PartialOrd_lt is (String, i32)'s PartialOrd::lt, lexical_ord's chain, on pointers. */
static bool
alloctests_sort_tests_TupleStringI32_PartialOrd_lt(const AlloctestsSortTestsTupleStringI32 *a, const AlloctestsSortTestsTupleStringI32 *b)
{
	StdOrdering c = alloctests_sort_tests_String_Ord_cmp(&a->f0, &b->f0);

	return c != STD_ORDERING_EQUAL ? c == STD_ORDERING_LESS : a->f1 < b->f1;
}

/*
 * alloctests_sort_tests_TupleStringI32_PartialOrd_lt_FnMut_call_mut is the fn
 * item TupleStringI32's PartialOrd::lt's FnMut shim, as the sort passes it for
 * is_less.
 */
static bool
alloctests_sort_tests_TupleStringI32_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return alloctests_sort_tests_TupleStringI32_PartialOrd_lt(a, b);
}

/* The stable sort and its shared parts at (String, i32). */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS TupleStringI32
#define STD_SORT_T AlloctestsSortTestsTupleStringI32
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TupleStringI32
#define STD_STABLE_SORT_T AlloctestsSortTestsTupleStringI32
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT alloctests_sort_tests_TupleStringI32_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

/*
 * std_String_PartialOrd_lt_FnMut_call_mut is the fn item String's
 * PartialOrd::lt's FnMut shim, as the sort passes it for is_less.
 */
static bool
std_String_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_String_PartialOrd_lt(a, b);
}

/* The stable sort, its shared parts, ptr and slice forms, the known good sort, and the tests' template at String. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS String
#define STD_SORT_T StdString
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS String
#define STD_STABLE_SORT_T StdString
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT std_String_PartialOrd_lt_FnMut_call_mut
#define STD_STABLE_SORT_WITH_BY
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS String
#define STD_PTR_T StdString
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS String
#define STD_SLICE_T StdString
#include "doltgres/std/slice_template.h"

ALLOCTESTS_SORT_TESTS_SORT_BY(String, StdString)

#define STD_KG_TS String
#define STD_KG_T StdString
#define STD_KG_LT(a, b) std_String_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS String
#define STD_ST_CMP(a, b) alloctests_sort_tests_String_Ord_cmp((a), (b))
#define STD_ST_EQ alloctests_sort_tests_String_PartialEq_eq
#define STD_ST_PAIR_TS TupleStringI32
#define STD_ST_PAIR_T AlloctestsSortTestsTupleStringI32
#define STD_ST_T StdString
#define STD_ST_LT(a, b) std_String_PartialOrd_lt((a), (b))
#define STD_ST_CLONE ALLOCTESTS_SORT_TESTS_STRING_CLONE
#define STD_ST_DROP std_String_drop
#define STD_ST_DEBUG std_String_Debug_fmt
#define STD_ST_IN_PLACE 0
#include "doltgres/std/test_sort_tests_template.h"

/*
 * alloctests_sort_ffi_types_F128_PartialOrd_lt_FnMut_call_mut is the fn item
 * F128's PartialOrd::lt's FnMut shim, as the sort passes it for is_less.
 */
static bool
alloctests_sort_ffi_types_F128_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return alloctests_sort_ffi_types_F128_PartialOrd_lt(a, b);
}

/* The stable sort, its shared parts, ptr and slice forms, the known good sort, and the tests' template at F128. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS F128
#define STD_SORT_T AlloctestsSortFfiTypesF128
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS F128
#define STD_STABLE_SORT_T AlloctestsSortFfiTypesF128
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT alloctests_sort_ffi_types_F128_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS F128
#define STD_PTR_T AlloctestsSortFfiTypesF128
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS F128
#define STD_SLICE_T AlloctestsSortFfiTypesF128
#include "doltgres/std/slice_template.h"

#define STD_KG_TS F128
#define STD_KG_T AlloctestsSortFfiTypesF128
#define STD_KG_LT(a, b) alloctests_sort_ffi_types_F128_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS F128
#define STD_ST_T AlloctestsSortFfiTypesF128
#define STD_ST_LT(a, b) alloctests_sort_ffi_types_F128_PartialOrd_lt((a), (b))
#define STD_ST_CLONE ALLOCTESTS_SORT_TESTS_COPY
#define STD_ST_DEBUG alloctests_sort_ffi_types_F128_Debug_fmt
#define STD_ST_IN_PLACE 0
#include "doltgres/std/test_sort_tests_template.h"

/*
 * alloctests_sort_ffi_types_FFIOneKibiByte_PartialOrd_lt_FnMut_call_mut is the
 * fn item FFIOneKibiByte's PartialOrd::lt's FnMut shim, as the sort passes it
 * for is_less.
 */
static bool
alloctests_sort_ffi_types_FFIOneKibiByte_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return alloctests_sort_ffi_types_FFIOneKibiByte_PartialOrd_lt(a, b);
}

/* The stable sort, its shared parts, ptr and slice forms, the known good sort, and the tests' template at FFIOneKibiByte. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS FFIOneKibiByte
#define STD_SORT_T AlloctestsSortFfiTypesFFIOneKibiByte
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS FFIOneKibiByte
#define STD_STABLE_SORT_T AlloctestsSortFfiTypesFFIOneKibiByte
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT alloctests_sort_ffi_types_FFIOneKibiByte_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS FFIOneKibiByte
#define STD_PTR_T AlloctestsSortFfiTypesFFIOneKibiByte
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS FFIOneKibiByte
#define STD_SLICE_T AlloctestsSortFfiTypesFFIOneKibiByte
#include "doltgres/std/slice_template.h"

#define STD_KG_TS FFIOneKibiByte
#define STD_KG_T AlloctestsSortFfiTypesFFIOneKibiByte
#define STD_KG_LT(a, b) alloctests_sort_ffi_types_FFIOneKibiByte_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS FFIOneKibiByte
#define STD_ST_T AlloctestsSortFfiTypesFFIOneKibiByte
#define STD_ST_LT(a, b) alloctests_sort_ffi_types_FFIOneKibiByte_PartialOrd_lt((a), (b))
#define STD_ST_CLONE ALLOCTESTS_SORT_TESTS_COPY
#define STD_ST_DEBUG alloctests_sort_ffi_types_FFIOneKibiByte_Debug_fmt
#define STD_ST_IN_PLACE 0
#include "doltgres/std/test_sort_tests_template.h"

/* AlloctestsSortTestsDynTraitVtable is DynTrait's trait table: the drop, size, and alignment every table starts with, then its supertrait Debug's fmt and get_val. */
typedef struct AlloctestsSortTestsDynTraitVtable
{
	void		(*drop) (void *value);
	size_t		size;
	size_t		align;
	StdFmtFn	fmt;
	int32		(*get_val) (const void *self);
} AlloctestsSortTestsDynTraitVtable;

/* AlloctestsSortTestsDynValA is DynValA, an i32 value. */
typedef struct AlloctestsSortTestsDynValA
{
	int32		value;
} AlloctestsSortTestsDynValA;

/* AlloctestsSortTestsDynValB is DynValB, a u64 value. */
typedef struct AlloctestsSortTestsDynValB
{
	uint64		value;
} AlloctestsSortTestsDynValB;

/* alloctests_sort_tests_DynValA_Debug_fmt is DynValA's derived Debug. */
static bool
alloctests_sort_tests_DynValA_Debug_fmt(const void *value, StdFormatter *f)
{
	const AlloctestsSortTestsDynValA *self = value;
	StdDebugStruct b = std_fmt_debug_struct(f, "DynValA", 7);

	std_DebugStruct_field(&b, "value", 5, &self->value, std_i32_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* alloctests_sort_tests_DynValB_Debug_fmt is DynValB's derived Debug. */
static bool
alloctests_sort_tests_DynValB_Debug_fmt(const void *value, StdFormatter *f)
{
	const AlloctestsSortTestsDynValB *self = value;
	StdDebugStruct b = std_fmt_debug_struct(f, "DynValB", 7);

	std_DebugStruct_field(&b, "value", 5, &self->value, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* alloctests_sort_tests_DynValA_DynTrait_get_val is DynValA's get_val: its value. */
static int32
alloctests_sort_tests_DynValA_DynTrait_get_val(const void *self_)
{
	const AlloctestsSortTestsDynValA *self = self_;

	return self->value;
}

/* alloctests_sort_tests_DynValB_DynTrait_get_val is DynValB's get_val: an i32 of its native bytes 0, 1, 6, and 7. */
static int32
alloctests_sort_tests_DynValB_DynTrait_get_val(const void *self_)
{
	const AlloctestsSortTestsDynValB *self = self_;
	uint8		bytes[8];
	uint8		picked[4];
	int32		result;

	memcpy(bytes, &self->value, sizeof(bytes));
	std_index_check(0, sizeof(bytes), RUST_FILE, 103, 29);
	picked[0] = bytes[0];
	std_index_check(1, sizeof(bytes), RUST_FILE, 103, 39);
	picked[1] = bytes[1];
	std_index_check(6, sizeof(bytes), RUST_FILE, 103, 49);
	picked[2] = bytes[6];
	std_index_check(7, sizeof(bytes), RUST_FILE, 103, 59);
	picked[3] = bytes[7];
	memcpy(&result, picked, sizeof(result));
	return result;
}

/* ALLOCTESTS_SORT_TESTS_DYN_VAL_A_DYN_TRAIT_VTABLE is DynValA's DynTrait table. */
static const AlloctestsSortTestsDynTraitVtable ALLOCTESTS_SORT_TESTS_DYN_VAL_A_DYN_TRAIT_VTABLE = {
	.size = sizeof(AlloctestsSortTestsDynValA),
	.align = _Alignof(AlloctestsSortTestsDynValA),
	.fmt = alloctests_sort_tests_DynValA_Debug_fmt,
	.get_val = alloctests_sort_tests_DynValA_DynTrait_get_val,
};

/* ALLOCTESTS_SORT_TESTS_DYN_VAL_B_DYN_TRAIT_VTABLE is DynValB's DynTrait table. */
static const AlloctestsSortTestsDynTraitVtable ALLOCTESTS_SORT_TESTS_DYN_VAL_B_DYN_TRAIT_VTABLE = {
	.size = sizeof(AlloctestsSortTestsDynValB),
	.align = _Alignof(AlloctestsSortTestsDynValB),
	.fmt = alloctests_sort_tests_DynValB_Debug_fmt,
	.get_val = alloctests_sort_tests_DynValB_DynTrait_get_val,
};

/* alloctests_sort_tests_RcDynTrait_get_val is get_val through an Rc<dyn DynTrait>'s table, on its value. */
static int32
alloctests_sort_tests_RcDynTrait_get_val(const StdRcDyn *self)
{
	const AlloctestsSortTestsDynTraitVtable *vtable = self->vtable;

	return vtable->get_val(std_Rc_deref(self->ptr, vtable->align));
}

/* alloctests_sort_tests_RcDynTrait_PartialOrd_lt is Rc<dyn DynTrait>'s PartialOrd::lt, dyn DynTrait's: cmp of get_val is Less, on pointers. */
static bool
alloctests_sort_tests_RcDynTrait_PartialOrd_lt(const StdRcDyn *a, const StdRcDyn *b)
{
	int32		a_val = alloctests_sort_tests_RcDynTrait_get_val(a);
	int32		b_val = alloctests_sort_tests_RcDynTrait_get_val(b);

	return std_i32_Ord_cmp(&a_val, &b_val) == STD_ORDERING_LESS;
}

/* alloctests_sort_tests_RcDynTrait_clone is Rc<dyn DynTrait>'s Clone into a destination: the strong count one higher, the pointer copied. */
static void
alloctests_sort_tests_RcDynTrait_clone(StdRcDyn *dst, const StdRcDyn *src)
{
	std_Rc_clone(src->ptr);
	*dst = *src;
}

/* alloctests_sort_tests_RcDynTrait_drop is Rc<dyn DynTrait>'s drop glue, through its table. */
static void
alloctests_sort_tests_RcDynTrait_drop(void *self_)
{
	StdRcDyn   *self = self_;
	const AlloctestsSortTestsDynTraitVtable *vtable = self->vtable;

	std_Rc_drop(self->ptr, vtable->size, vtable->align, vtable->drop);
}

/* alloctests_sort_tests_RcDynTrait_Debug_fmt is Rc<dyn DynTrait>'s Debug, its value's through its table. */
static bool
alloctests_sort_tests_RcDynTrait_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdRcDyn *self = value;
	const AlloctestsSortTestsDynTraitVtable *vtable = self->vtable;

	return std_Rc_Debug_fmt(self->ptr, vtable->align, vtable->fmt, f);
}

/* alloctests_sort_tests_dyn_trait_from_i32_into is dyn_trait_from_i32 as the Fn(i32) -> Rc<dyn DynTrait> a test takes: a DynValA for an even value, else a DynValB of it extended. */
static void
alloctests_sort_tests_dyn_trait_from_i32_into(void *captures, int32 val, StdRcDyn *result)
{
	if (STD_REM_FOLDED(i32, val, 2) == 0)
	{
		AlloctestsSortTestsDynValA a = {val};

		result->ptr = std_Rc_new(&a, sizeof(a), _Alignof(AlloctestsSortTestsDynValA));
		result->vtable = &ALLOCTESTS_SORT_TESTS_DYN_VAL_A_DYN_TRAIT_VTABLE;
	}
	else
	{
		AlloctestsSortTestsDynValB b = {alloctests_sort_tests_extend_i32_to_u64(val)};

		result->ptr = std_Rc_new(&b, sizeof(b), _Alignof(AlloctestsSortTestsDynValB));
		result->vtable = &ALLOCTESTS_SORT_TESTS_DYN_VAL_B_DYN_TRAIT_VTABLE;
	}
}

/*
 * alloctests_sort_tests_RcDynTrait_PartialOrd_lt_FnMut_call_mut is the fn item
 * RcDynTrait's PartialOrd::lt's FnMut shim, as the sort passes it for is_less.
 */
static bool
alloctests_sort_tests_RcDynTrait_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return alloctests_sort_tests_RcDynTrait_PartialOrd_lt(a, b);
}

/* The stable sort, its shared parts, ptr and slice forms, the known good sort, and the tests' template at RcDynTrait. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS RcDynTrait
#define STD_SORT_T StdRcDyn
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS RcDynTrait
#define STD_STABLE_SORT_T StdRcDyn
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT alloctests_sort_tests_RcDynTrait_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS RcDynTrait
#define STD_PTR_T StdRcDyn
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS RcDynTrait
#define STD_SLICE_T StdRcDyn
#include "doltgres/std/slice_template.h"

#define STD_KG_TS RcDynTrait
#define STD_KG_T StdRcDyn
#define STD_KG_LT(a, b) alloctests_sort_tests_RcDynTrait_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS RcDynTrait
#define STD_ST_T StdRcDyn
#define STD_ST_LT(a, b) alloctests_sort_tests_RcDynTrait_PartialOrd_lt((a), (b))
#define STD_ST_CLONE alloctests_sort_tests_RcDynTrait_clone
#define STD_ST_DROP alloctests_sort_tests_RcDynTrait_drop
#define STD_ST_DEBUG alloctests_sort_tests_RcDynTrait_Debug_fmt
#define STD_ST_IN_PLACE 0
#include "doltgres/std/test_sort_tests_template.h"

/* AlloctestsSortTestsCompCount is CompCount, repr(C): a value and how many comparisons saw it. */
typedef struct AlloctestsSortTestsCompCount
{
	int32		val;
	uint32		comp_count;
} AlloctestsSortTestsCompCount;

/* The stable sort's sort_by and its shared parts at CompCount, which has no Ord. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS CompCount
#define STD_SORT_T AlloctestsSortTestsCompCount
#define STD_SORT_FREEZE 0
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS CompCount
#define STD_STABLE_SORT_T AlloctestsSortTestsCompCount
#define STD_STABLE_SORT_FREEZE 0
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY
#include "doltgres/std/stable_sort_template.h"

ALLOCTESTS_SORT_TESTS_SORT_BY(CompCount, AlloctestsSortTestsCompCount)

/* alloctests_sort_tests_collect_comp_counts_frame_0_t is collect_comp_counts' state across its pad: the pattern it consumes and the vector it fills, while live. */
typedef struct alloctests_sort_tests_collect_comp_counts_frame_0_t
{
	StdVec		pattern;
	bool		pattern_live;
	StdVec	   *result;
	bool		result_live;
} alloctests_sort_tests_collect_comp_counts_frame_0_t;

/* alloctests_sort_tests_collect_comp_counts_body_0 is the collect: CompCount is wider than i32, so a new vector of the pattern's length, filled, then the pattern freed. */
static void
alloctests_sort_tests_collect_comp_counts_body_0(void *frame_)
{
	alloctests_sort_tests_collect_comp_counts_frame_0_t *frame = frame_;
	StdSizeHint hint = {frame->pattern.len, true, frame->pattern.len};

	std_vec_with_capacity_trusted(frame->result, hint, sizeof(AlloctestsSortTestsCompCount),
								  _Alignof(AlloctestsSortTestsCompCount));
	frame->result_live = true;
	std_vec_extend_trusted_reserve(frame->result, hint, sizeof(AlloctestsSortTestsCompCount),
								   _Alignof(AlloctestsSortTestsCompCount));
	for (size_t k = 0, k_len = frame->pattern.len; k < k_len; k++)
	{
		AlloctestsSortTestsCompCount c = {STD_VEC_AT(&frame->pattern, int32, k), 0};

		STD_VEC_AT(frame->result, AlloctestsSortTestsCompCount, frame->result->len) = c;
		frame->result->len = std_wrapping_add_usize(frame->result->len, 1);
	}
	frame->pattern_live = false;
	std_vec_free_Checked(&frame->pattern, sizeof(int32), _Alignof(int32));
	frame->result_live = false;
}

/* alloctests_sort_tests_collect_comp_counts_pad_0 is the collect's pad: the vector so far and the pattern, while live. */
static void
alloctests_sort_tests_collect_comp_counts_pad_0(void *frame_)
{
	alloctests_sort_tests_collect_comp_counts_frame_0_t *frame = frame_;

	if (frame->result_live)
		std_vec_free_Checked(frame->result, sizeof(AlloctestsSortTestsCompCount),
							 _Alignof(AlloctestsSortTestsCompCount));
	if (frame->pattern_live)
		std_vec_free_Checked(&frame->pattern, sizeof(int32), _Alignof(int32));
}

/*
 * alloctests_sort_tests_collect_comp_counts is
 * pattern.into_iter().map(|val| CompCount::new(val)).collect(), a Vec's
 * from_iter of a TrustedLen iterator: it takes the pattern and fills result.
 */
static void
alloctests_sort_tests_collect_comp_counts(StdVec *pattern, StdVec *result)
{
	alloctests_sort_tests_collect_comp_counts_frame_0_t frame = {.pattern = *pattern, .pattern_live = true, .result = result};

	std_landing(&frame, alloctests_sort_tests_collect_comp_counts_body_0, alloctests_sort_tests_collect_comp_counts_pad_0);
}

/* alloctests_sort_tests_comp_count_total is the u64 sum of each CompCount's comparisons, Sum's fold with its overflow check. */
static uint64
alloctests_sort_tests_comp_count_total(const StdVec *test_input)
{
	uint64		total = 0;

	for (size_t k = 0, k_len = test_input->len; k < k_len; k++)
		total = std_add_u64(total, (uint64) STD_VEC_AT(test_input, AlloctestsSortTestsCompCount, k).comp_count,
							STD_ACCUM_FILE, 206, 1);
	return total;
}

/* alloctests_sort_tests_comp_count_sum is the i64 sum of each CompCount's value, Sum's fold with its overflow check. */
static int64
alloctests_sort_tests_comp_count_sum(const StdVec *test_input)
{
	int64		sum = 0;

	for (size_t k = 0, k_len = test_input->len; k < k_len; k++)
		sum = std_add_i64(sum, (int64) STD_VEC_AT(test_input, AlloctestsSortTestsCompCount, k).val, STD_ACCUM_FILE,
						  206, 1);
	return sum;
}

/* alloctests_sort_tests_observable_is_less_closure_1 is observable_is_less's comparison: both counted, then the values' cmp. */
static StdOrdering
alloctests_sort_tests_observable_is_less_closure_1(void *captures, const void *a_, const void *b_)
{
	uint64	   *comp_count_global = captures;
	AlloctestsSortTestsCompCount *a = (AlloctestsSortTestsCompCount *) a_;
	AlloctestsSortTestsCompCount *b = (AlloctestsSortTestsCompCount *) b_;

	a->comp_count = STD_ADD(u32, a->comp_count, 1, 580, 30);
	b->comp_count = STD_ADD(u32, b->comp_count, 1, 581, 30);
	*comp_count_global = STD_ADD(u64, *comp_count_global, 1, 582, 9);
	return std_i32_Ord_cmp(&a->val, &b->val);
}

/* alloctests_sort_tests_observable_is_less_frame_0_t is observable_is_less's state across its pad: the test input, while live. */
typedef struct alloctests_sort_tests_observable_is_less_frame_0_t
{
	size_t		len;
	void		(*pattern_fn) (size_t len, StdVec *result);
	StdVec		test_input;
	bool		test_input_live;
} alloctests_sort_tests_observable_is_less_frame_0_t;

/* alloctests_sort_tests_observable_is_less_body_0 is observable_is_less's body: each comparison the sort makes is seen in the slice. */
static void
alloctests_sort_tests_observable_is_less_body_0(void *frame_)
{
	alloctests_sort_tests_observable_is_less_frame_0_t *frame = frame_;
	StdVec		pattern;
	uint64		comp_count_global = 0;
	uint64		total_inner;
	uint64		doubled;

	frame->pattern_fn(frame->len, &pattern);
	alloctests_sort_tests_collect_comp_counts(&pattern, &frame->test_input);
	frame->test_input_live = true;
	alloctests_sort_tests_stable_SortImpl_sort_by_CompCount(frame->test_input.ptr, frame->test_input.len,
															alloctests_sort_tests_observable_is_less_closure_1,
															&comp_count_global, NULL);
	total_inner = alloctests_sort_tests_comp_count_total(&frame->test_input);
	doubled = STD_MUL(u64, comp_count_global, 2, 589, 29);
	STD_ASSERT_EQ(589, 5, total_inner == doubled, &total_inner, std_u64_Debug_fmt, &doubled, std_u64_Debug_fmt);
	frame->test_input_live = false;
	std_vec_free_Checked(&frame->test_input, sizeof(AlloctestsSortTestsCompCount),
						 _Alignof(AlloctestsSortTestsCompCount));
}

/* alloctests_sort_tests_observable_is_less_pad_0 is observable_is_less's pad: the test input, while live. */
static void
alloctests_sort_tests_observable_is_less_pad_0(void *frame_)
{
	alloctests_sort_tests_observable_is_less_frame_0_t *frame = frame_;

	if (frame->test_input_live)
		std_vec_free_Checked(&frame->test_input, sizeof(AlloctestsSortTestsCompCount),
							 _Alignof(AlloctestsSortTestsCompCount));
}

/* alloctests_sort_tests_observable_is_less checks that every comparison the sort makes is observable in the slice. */
static void
alloctests_sort_tests_observable_is_less(size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_observable_is_less_frame_0_t frame = {.len = len, .pattern_fn = pattern_fn};

	std_landing(&frame, alloctests_sort_tests_observable_is_less_body_0, alloctests_sort_tests_observable_is_less_pad_0);
}

/* AlloctestsSortTestsCalcCompsRequiredCompCountClosure0 is calc_comps_required's closure's captures at CompCount: the counter and the comparison, by reference. */
typedef struct AlloctestsSortTestsCalcCompsRequiredCompCountClosure0
{
	uint32	   *comp_counter;
	StdOrdering (*cmp_fn) (void *captures, const AlloctestsSortTestsCompCount *a,
						   const AlloctestsSortTestsCompCount *b);
	void	   *cmp_fn_captures;
} AlloctestsSortTestsCalcCompsRequiredCompCountClosure0;

/* alloctests_sort_tests_calc_comps_required_CompCount_closure_0 is calc_comps_required's closure at CompCount: one more comparison counted, then cmp_fn's. */
static StdOrdering
alloctests_sort_tests_calc_comps_required_CompCount_closure_0(void *captures, const void *a, const void *b)
{
	AlloctestsSortTestsCalcCompsRequiredCompCountClosure0 *closure = captures;

	*closure->comp_counter = STD_ADD(u32, *closure->comp_counter, 1, 183, 9);
	return closure->cmp_fn(closure->cmp_fn_captures, a, b);
}

/*
 * alloctests_sort_tests_calc_comps_required_CompCount is calc_comps_required
 * at CompCount: the comparisons the stable sort_by makes on v with cmp_fn,
 * counted. Its cmp_fn is a closure without captures or drop glue, so it
 * holds nothing to drop on an unwind.
 */
static uint32
alloctests_sort_tests_calc_comps_required_CompCount(AlloctestsSortTestsCompCount *v, size_t len,
													StdOrdering (*cmp_fn) (void *captures,
																		   const AlloctestsSortTestsCompCount *a,
																		   const AlloctestsSortTestsCompCount *b),
													void *cmp_fn_captures)
{
	uint32		comp_counter = 0;
	AlloctestsSortTestsCalcCompsRequiredCompCountClosure0 closure = {&comp_counter, cmp_fn, cmp_fn_captures};

	alloctests_sort_tests_stable_SortImpl_sort_by_CompCount(v, len,
															alloctests_sort_tests_calc_comps_required_CompCount_closure_0,
															&closure, NULL);
	return comp_counter;
}

/* alloctests_sort_tests_panic_observable_is_less_closure_1 is panic_observable_is_less's first comparison, |a, b| a.val.cmp(&b.val). */
static StdOrdering
alloctests_sort_tests_panic_observable_is_less_closure_1(void *captures, const AlloctestsSortTestsCompCount *a,
														 const AlloctestsSortTestsCompCount *b)
{
	return std_i32_Ord_cmp(&a->val, &b->val);
}

/* AlloctestsSortTestsPanicObservableIsLessClosure3 is panic_observable_is_less's sorting comparison's captures: the counter and the threshold, by reference. */
typedef struct AlloctestsSortTestsPanicObservableIsLessClosure3
{
	uint64	   *comp_count_global;
	const uint64 *panic_threshold;
} AlloctestsSortTestsPanicObservableIsLessClosure3;

/* alloctests_sort_tests_panic_observable_is_less_closure_3 is the sorting comparison: a panic at the threshold, else both counted and the values' cmp. */
static StdOrdering
alloctests_sort_tests_panic_observable_is_less_closure_3(void *captures, const void *a_, const void *b_)
{
	AlloctestsSortTestsPanicObservableIsLessClosure3 *closure = captures;
	AlloctestsSortTestsCompCount *a = (AlloctestsSortTestsCompCount *) a_;
	AlloctestsSortTestsCompCount *b = (AlloctestsSortTestsCompCount *) b_;

	if (*closure->comp_count_global == *closure->panic_threshold)
		STD_PANIC(669, 17, "explicit panic");
	a->comp_count = STD_ADD(u32, a->comp_count, 1, 672, 34);
	b->comp_count = STD_ADD(u32, b->comp_count, 1, 673, 34);
	*closure->comp_count_global = STD_ADD(u64, *closure->comp_count_global, 1, 674, 13);
	return std_i32_Ord_cmp(&a->val, &b->val);
}

/* AlloctestsSortTestsPanicObservableIsLessClosure2 is catch_unwind's body's captures: the test input, the counter, and the threshold, by reference. */
typedef struct AlloctestsSortTestsPanicObservableIsLessClosure2
{
	StdVec	   *test_input;
	uint64	   *comp_count_global;
	const uint64 *panic_threshold;
} AlloctestsSortTestsPanicObservableIsLessClosure2;

/* alloctests_sort_tests_panic_observable_is_less_closure_2 is catch_unwind's body: the test input's sort_by with the sorting comparison. */
static void
alloctests_sort_tests_panic_observable_is_less_closure_2(void *captures)
{
	AlloctestsSortTestsPanicObservableIsLessClosure2 *closure = captures;
	AlloctestsSortTestsPanicObservableIsLessClosure3 inner = {closure->comp_count_global, closure->panic_threshold};

	alloctests_sort_tests_stable_SortImpl_sort_by_CompCount(closure->test_input->ptr, closure->test_input->len,
															alloctests_sort_tests_panic_observable_is_less_closure_3,
															&inner, NULL);
}

/* alloctests_sort_tests_panic_observable_is_less_frame_0_t is panic_observable_is_less's state across its pad: the vectors and the caught panic, while live. */
typedef struct alloctests_sort_tests_panic_observable_is_less_frame_0_t
{
	size_t		len;
	void		(*pattern_fn) (size_t len, StdVec *result);
	StdVec		test_input;
	bool		test_input_live;
	StdVec		clone;
	bool		clone_live;
	StdVec		random;
	bool		random_live;
	StdCaught	res;
	bool		res_live;
} alloctests_sort_tests_panic_observable_is_less_frame_0_t;

/* alloctests_sort_tests_panic_observable_is_less_body_0 is panic_observable_is_less's body: comparisons stay observable when one panics. */
static void
alloctests_sort_tests_panic_observable_is_less_body_0(void *frame_)
{
	alloctests_sort_tests_panic_observable_is_less_frame_0_t *frame = frame_;
	StdVec		pattern;
	int64		sum_before;
	int64		sum_after;
	uint32		required_comps;
	uint64		panic_threshold;
	uint64		comp_count_global = 0;
	uint64		total_inner;
	uint64		doubled;
	bool		returned;

	frame->pattern_fn(frame->len, &pattern);
	alloctests_sort_tests_collect_comp_counts(&pattern, &frame->test_input);
	frame->test_input_live = true;
	sum_before = alloctests_sort_tests_comp_count_sum(&frame->test_input);
	std_vec_clone(&frame->clone, &frame->test_input, sizeof(AlloctestsSortTestsCompCount),
				  _Alignof(AlloctestsSortTestsCompCount));
	frame->clone_live = true;
	required_comps = alloctests_sort_tests_calc_comps_required_CompCount(frame->clone.ptr, frame->clone.len,
																		 alloctests_sort_tests_panic_observable_is_less_closure_1,
																		 NULL);
	frame->clone_live = false;
	std_vec_free_Checked(&frame->clone, sizeof(AlloctestsSortTestsCompCount), _Alignof(AlloctestsSortTestsCompCount));
	alloctests_sort_patterns_random_uniform_RangeInclusiveI32(1, 1, (int32) required_comps, &frame->random);
	frame->random_live = true;
	std_index_check(0, frame->random.len, RUST_FILE, 659, 81);
	panic_threshold = STD_SUB(u64, (uint64) (int64) STD_VEC_AT(&frame->random, int32, 0), 1, 659, 27);
	frame->random_live = false;
	std_vec_free_Checked(&frame->random, sizeof(int32), _Alignof(int32));
	{
		AlloctestsSortTestsPanicObservableIsLessClosure2 closure = {&frame->test_input, &comp_count_global,
		&panic_threshold};

		returned = std_catch_unwind(alloctests_sort_tests_panic_observable_is_less_closure_2, &closure, &frame->res);
	}
	frame->res_live = !returned;
	STD_ASSERT(680, 5, !returned, "res.is_err()");
	total_inner = alloctests_sort_tests_comp_count_total(&frame->test_input);
	doubled = STD_MUL(u64, comp_count_global, 2, 684, 29);
	STD_ASSERT_EQ(684, 5, total_inner == doubled, &total_inner, std_u64_Debug_fmt, &doubled, std_u64_Debug_fmt);
	sum_after = alloctests_sort_tests_comp_count_sum(&frame->test_input);
	STD_ASSERT_EQ(689, 5, sum_before == sum_after, &sum_before, std_i64_Debug_fmt, &sum_after, std_i64_Debug_fmt);
	if (frame->res_live)
	{
		frame->res_live = false;
		std_Caught_drop_Checked(&frame->res);
	}
	frame->test_input_live = false;
	std_vec_free_Checked(&frame->test_input, sizeof(AlloctestsSortTestsCompCount),
						 _Alignof(AlloctestsSortTestsCompCount));
}

/* alloctests_sort_tests_panic_observable_is_less_pad_0 is panic_observable_is_less's pad: the caught panic and the vectors, while live. */
static void
alloctests_sort_tests_panic_observable_is_less_pad_0(void *frame_)
{
	alloctests_sort_tests_panic_observable_is_less_frame_0_t *frame = frame_;

	if (frame->res_live)
		std_Caught_drop_Checked(&frame->res);
	if (frame->random_live)
		std_vec_free_Checked(&frame->random, sizeof(int32), _Alignof(int32));
	if (frame->clone_live)
		std_vec_free_Checked(&frame->clone, sizeof(AlloctestsSortTestsCompCount),
							 _Alignof(AlloctestsSortTestsCompCount));
	if (frame->test_input_live)
		std_vec_free_Checked(&frame->test_input, sizeof(AlloctestsSortTestsCompCount),
							 _Alignof(AlloctestsSortTestsCompCount));
}

/* alloctests_sort_tests_panic_observable_is_less checks that comparisons stay observable when one panics. */
static void
alloctests_sort_tests_panic_observable_is_less(size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_observable_is_less_frame_0_t frame = {.len = len, .pattern_fn = pattern_fn};

	std_landing(&frame, alloctests_sort_tests_panic_observable_is_less_body_0,
				alloctests_sort_tests_panic_observable_is_less_pad_0);
}

/* AlloctestsSortTestsBoxI32 is a Box<i32>: the block holding the value. */
typedef int32 *AlloctestsSortTestsBoxI32;

/* alloctests_sort_tests_BoxI32_PartialOrd_lt is Box<i32>'s PartialOrd::lt, its values', on pointers to the boxes. */
static bool
alloctests_sort_tests_BoxI32_PartialOrd_lt(const AlloctestsSortTestsBoxI32 *a, const AlloctestsSortTestsBoxI32 *b)
{
	return std_i32_PartialOrd_lt(*a, *b);
}

/* alloctests_sort_tests_BoxI32_Debug_fmt is Box<i32>'s Debug, its value's. */
static bool
alloctests_sort_tests_BoxI32_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_i32_Debug_fmt(*(int32 *const *) value, f);
}

/* alloctests_sort_tests_BoxI32_drop is Box<i32>'s drop glue: its block freed. */
static void
alloctests_sort_tests_BoxI32_drop(void *self)
{
	std_free(*(int32 **) self, sizeof(int32), _Alignof(int32));
}

/* alloctests_sort_tests_BoxI32_clone is Box<i32>'s Clone into a destination: a new box of the value. */
static void
alloctests_sort_tests_BoxI32_clone(AlloctestsSortTestsBoxI32 *dst, const AlloctestsSortTestsBoxI32 *src)
{
	int32	   *boxed = std_box_new_uninit_Checked(sizeof(int32), _Alignof(int32));

	*boxed = **src;
	*dst = boxed;
}

/* alloctests_sort_tests_Box_new_into is Box::new as the Fn(i32) -> Box<i32> a test takes, a fn item with no captures. */
static void
alloctests_sort_tests_Box_new_into(void *captures, int32 val, AlloctestsSortTestsBoxI32 *result)
{
	int32	   *boxed = std_box_new_uninit_Checked(sizeof(int32), _Alignof(int32));

	*boxed = val;
	*result = boxed;
}

/*
 * alloctests_sort_tests_BoxI32_PartialOrd_lt_FnMut_call_mut is the fn item
 * BoxI32's PartialOrd::lt's FnMut shim, as the sort passes it for is_less.
 */
static bool
alloctests_sort_tests_BoxI32_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return alloctests_sort_tests_BoxI32_PartialOrd_lt(a, b);
}

/* The stable sort, its shared parts, ptr and slice forms, the known good sort, and the tests' template at BoxI32. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS BoxI32
#define STD_SORT_T AlloctestsSortTestsBoxI32
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS BoxI32
#define STD_STABLE_SORT_T AlloctestsSortTestsBoxI32
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT alloctests_sort_tests_BoxI32_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

#define STD_PTR_SCOPE static
#define STD_PTR_DECLARE
#define STD_PTR_DEFINE
#define STD_PTR_TS BoxI32
#define STD_PTR_T AlloctestsSortTestsBoxI32
#include "doltgres/std/ptr_template.h"

#define STD_SLICE_SCOPE static
#define STD_SLICE_DECLARE
#define STD_SLICE_DEFINE
#define STD_SLICE_TS BoxI32
#define STD_SLICE_T AlloctestsSortTestsBoxI32
#include "doltgres/std/slice_template.h"

#define STD_KG_TS BoxI32
#define STD_KG_T AlloctestsSortTestsBoxI32
#define STD_KG_LT(a, b) alloctests_sort_tests_BoxI32_PartialOrd_lt((a), (b))
#include "doltgres/std/test_sort_known_good_template.h"

#define STD_ST_TS BoxI32
#define STD_ST_T AlloctestsSortTestsBoxI32
#define STD_ST_LT(a, b) alloctests_sort_tests_BoxI32_PartialOrd_lt((a), (b))
#define STD_ST_CLONE alloctests_sort_tests_BoxI32_clone
#define STD_ST_DROP alloctests_sort_tests_BoxI32_drop
#define STD_ST_DEBUG alloctests_sort_tests_BoxI32_Debug_fmt
#define STD_ST_IN_PLACE 0
#include "doltgres/std/test_sort_tests_template.h"

/* The stable sort at (), which returns at once. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS Unit
#define STD_STABLE_SORT_UNIT
#include "doltgres/std/stable_sort_template.h"

#define STD_ST_TS Unit
#define STD_ST_UNIT
#include "doltgres/std/test_sort_tests_template.h"

/* ALLOCTESTS_SORT_TESTS_TEST_LENGTHS is TEST_LENGTHS outside Miri and emscripten, the lengths the tests sort. */
static const size_t ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 17, 20, 24, 30, 32, 33,
35, 50, 100, 200, 500, 1000, 2048, 5000, 10000, 100000, 1100000};

/* alloctests_sort_patterns_random_closure is patterns::random as the Fn(usize) -> Vec<i32> a test takes, a fn item with no captures. */
static void
alloctests_sort_patterns_random_closure(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random(len, result);
}

/* alloctests_sort_patterns_ascending_closure is patterns::ascending as the Fn(usize) -> Vec<i32> a test takes, a fn item with no captures. */
static void
alloctests_sort_patterns_ascending_closure(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_ascending(len, result);
}

/* alloctests_sort_patterns_descending_closure is patterns::descending as the Fn(usize) -> Vec<i32> a test takes, a fn item with no captures. */
static void
alloctests_sort_patterns_descending_closure(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_descending(len, result);
}

/* alloctests_sort_patterns_all_equal_closure is patterns::all_equal as the Fn(usize) -> Vec<i32> a test takes, a fn item with no captures. */
static void
alloctests_sort_patterns_all_equal_closure(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_all_equal(len, result);
}

/* alloctests_sort_patterns_pipe_organ_closure is patterns::pipe_organ as the Fn(usize) -> Vec<i32> a test takes, a fn item with no captures. */
static void
alloctests_sort_patterns_pipe_organ_closure(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_pipe_organ(len, result);
}

/* test_alloctests_sort_tests_stable_basic is the test basic: check_is_sorted on short slices of () and of i32. */
void
test_alloctests_sort_tests_stable_basic(void)
{
	alloctests_sort_tests_check_is_sorted_I32(STD_DANGLING_FOR(_Alignof(int32)), 0);
	alloctests_sort_tests_check_is_sorted_Unit(0);
	alloctests_sort_tests_check_is_sorted_Unit(1);
	alloctests_sort_tests_check_is_sorted_Unit(2);
	alloctests_sort_tests_check_is_sorted_Unit(3);
	alloctests_sort_tests_check_is_sorted_I32(STD_DANGLING_FOR(_Alignof(int32)), 0);
	{
		int32		v[] = {77};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {2, 3};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {2, 3, 6};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {2, 3, 99, 6};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {2, 7709, 400, 90932};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {15, -1, 3, -1, -3, -1, 7};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
}

/* alloctests_sort_tests_SliceI32_Debug_fmt is [i32]'s Debug: a list of the values. */
static bool
alloctests_sort_tests_SliceI32_Debug_fmt(const int32 *self, size_t self_len, StdFormatter *f)
{
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = self_len; k < k_len; k++)
		std_DebugList_entry(&list, &self[k], std_i32_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* alloctests_sort_tests_RefSliceI32_Debug_fmt is &[i32]'s Debug, its slice's. */
static bool
alloctests_sort_tests_RefSliceI32_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdBytes *self = value;

	return alloctests_sort_tests_SliceI32_Debug_fmt((const int32 *) self->ptr, self->len, f);
}

/* alloctests_sort_tests_Array11I32_Debug_fmt is [i32; 11]'s Debug, its slice's. */
static bool
alloctests_sort_tests_Array11I32_Debug_fmt(const void *value, StdFormatter *f)
{
	return alloctests_sort_tests_SliceI32_Debug_fmt(value, 11, f);
}

/* alloctests_sort_tests_SliceI32_PartialEqSliceI32_eq is [i32]'s PartialEq: the same length and values. */
static bool
alloctests_sort_tests_SliceI32_PartialEqSliceI32_eq(const int32 *self, size_t self_len, const int32 *other,
													size_t other_len)
{
	return self_len == other_len && (self_len == 0 || memcmp(self, other, std_wrapping_mul_usize(self_len, sizeof(int32))) == 0);
}

/* alloctests_sort_tests_VecI32_PartialEqSliceI32_eq is Vec<i32>'s PartialEq<[i32]>, its slices'. */
static bool
alloctests_sort_tests_VecI32_PartialEqSliceI32_eq(const StdVec *self, const int32 *other, size_t other_len)
{
	return alloctests_sort_tests_SliceI32_PartialEqSliceI32_eq(self->ptr, self->len, other, other_len);
}

/* alloctests_sort_tests_Array11I32_PartialEqArray11I32_eq is [i32; 11]'s PartialEq, its slices'. */
static bool
alloctests_sort_tests_Array11I32_PartialEqArray11I32_eq(const int32 *self, const int32 *other)
{
	return alloctests_sort_tests_SliceI32_PartialEqSliceI32_eq(self, 11, other, 11);
}

/* alloctests_sort_tests_VecI32_PartialEqArray11I32_eq is Vec<i32>'s PartialEq<[i32; 11]>, its slices'. */
static bool
alloctests_sort_tests_VecI32_PartialEqArray11I32_eq(const StdVec *self, const int32 *other)
{
	return alloctests_sort_tests_SliceI32_PartialEqSliceI32_eq(self->ptr, self->len, other, 11);
}

/* test_alloctests_sort_tests_stable_fixed_seed is the test fixed_seed: the seed is the same each time it is asked for. */
void
test_alloctests_sort_tests_stable_fixed_seed(void)
{
	uint64		fixed_seed_a = alloctests_sort_patterns_get_or_init_rand_seed();
	uint64		fixed_seed_b = alloctests_sort_patterns_get_or_init_rand_seed();

	STD_ASSERT_EQ(304, 5, fixed_seed_a == fixed_seed_b, &fixed_seed_a, std_u64_Debug_fmt, &fixed_seed_b,
				  std_u64_Debug_fmt);
}

/* alloctests_sort_tests_fixed_seed_rand_vec_prefix_frame_0_t is the test's state across its pad: its two vectors, while live. */
typedef struct alloctests_sort_tests_fixed_seed_rand_vec_prefix_frame_0_t
{
	StdVec		vec_rand_len_5;
	bool		vec_rand_len_5_live;
	StdVec		vec_rand_len_7;
	bool		vec_rand_len_7_live;
} alloctests_sort_tests_fixed_seed_rand_vec_prefix_frame_0_t;

/* alloctests_sort_tests_fixed_seed_rand_vec_prefix_body_0 is the test: a random pattern of 5 is the prefix of one of 7. */
static void
alloctests_sort_tests_fixed_seed_rand_vec_prefix_body_0(void *frame_)
{
	alloctests_sort_tests_fixed_seed_rand_vec_prefix_frame_0_t *frame = frame_;
	StdBytes	prefix;

	alloctests_sort_patterns_random(5, &frame->vec_rand_len_5);
	frame->vec_rand_len_5_live = true;
	alloctests_sort_patterns_random(7, &frame->vec_rand_len_7);
	frame->vec_rand_len_7_live = true;
	std_range_to_check(5, frame->vec_rand_len_7.len, RUST_FILE, 311, 46);
	prefix.ptr = frame->vec_rand_len_7.ptr;
	prefix.len = 5;
	STD_ASSERT_EQ(311, 5, alloctests_sort_tests_VecI32_PartialEqSliceI32_eq(&frame->vec_rand_len_5,
																			(const int32 *) prefix.ptr, prefix.len),
				  &frame->vec_rand_len_5, alloctests_sort_tests_VecI32_Debug_fmt, &prefix,
				  alloctests_sort_tests_RefSliceI32_Debug_fmt);
	frame->vec_rand_len_7_live = false;
	std_vec_free_Checked(&frame->vec_rand_len_7, sizeof(int32), _Alignof(int32));
	frame->vec_rand_len_5_live = false;
	std_vec_free_Checked(&frame->vec_rand_len_5, sizeof(int32), _Alignof(int32));
}

/* alloctests_sort_tests_fixed_seed_rand_vec_prefix_pad_0 is the test's pad: its vectors, while live. */
static void
alloctests_sort_tests_fixed_seed_rand_vec_prefix_pad_0(void *frame_)
{
	alloctests_sort_tests_fixed_seed_rand_vec_prefix_frame_0_t *frame = frame_;

	if (frame->vec_rand_len_7_live)
		std_vec_free_Checked(&frame->vec_rand_len_7, sizeof(int32), _Alignof(int32));
	if (frame->vec_rand_len_5_live)
		std_vec_free_Checked(&frame->vec_rand_len_5, sizeof(int32), _Alignof(int32));
}

/* test_alloctests_sort_tests_stable_fixed_seed_rand_vec_prefix is the test fixed_seed_rand_vec_prefix. */
void
test_alloctests_sort_tests_stable_fixed_seed_rand_vec_prefix(void)
{
	alloctests_sort_tests_fixed_seed_rand_vec_prefix_frame_0_t frame = {0};

	std_landing(&frame, alloctests_sort_tests_fixed_seed_rand_vec_prefix_body_0,
				alloctests_sort_tests_fixed_seed_rand_vec_prefix_pad_0);
}

/* alloctests_sort_tests_int_edge_frame_0_t is int_edge's state across its pad: the large pattern, while live. */
typedef struct alloctests_sort_tests_int_edge_frame_0_t
{
	StdVec		large;
	bool		large_live;
} alloctests_sort_tests_int_edge_frame_0_t;

/* alloctests_sort_tests_int_edge_body_0 is the test: slices that hold the integer types' extremes, and a large one with i32's. */
static void
alloctests_sort_tests_int_edge_body_0(void *frame_)
{
	alloctests_sort_tests_int_edge_frame_0_t *frame = frame_;
	uint64		u64_max_3 = std_wrapping_sub_u64(PG_UINT64_MAX, 3);
	int32		max = PG_INT32_MAX;
	int32		min = PG_INT32_MIN;

	{
		int32		v[] = {PG_INT32_MIN, PG_INT32_MAX};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {PG_INT32_MAX, PG_INT32_MIN};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {PG_INT32_MIN, 3};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {PG_INT32_MIN, -3};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {PG_INT32_MIN, -3, PG_INT32_MAX};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {PG_INT32_MIN, -3, PG_INT32_MAX, PG_INT32_MIN, 5};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		int32		v[] = {PG_INT32_MAX, 3, PG_INT32_MIN, 5, PG_INT32_MIN, -3, 60, 200, 50, 7, 10};

		alloctests_sort_tests_check_is_sorted_I32(v, lengthof(v));
	}
	{
		uint64		v[] = {0, PG_UINT64_MAX};

		alloctests_sort_tests_check_is_sorted_U64(v, lengthof(v));
	}
	{
		uint64		v[] = {PG_UINT64_MAX, 0};

		alloctests_sort_tests_check_is_sorted_U64(v, lengthof(v));
	}
	{
		uint64		v[] = {0, 3};

		alloctests_sort_tests_check_is_sorted_U64(v, lengthof(v));
	}
	{
		uint64		v[] = {0, u64_max_3};

		alloctests_sort_tests_check_is_sorted_U64(v, lengthof(v));
	}
	{
		uint64		v[] = {0, u64_max_3, PG_UINT64_MAX};

		alloctests_sort_tests_check_is_sorted_U64(v, lengthof(v));
	}
	{
		uint64		v[] = {0, u64_max_3, PG_UINT64_MAX, 0, 5};

		alloctests_sort_tests_check_is_sorted_U64(v, lengthof(v));
	}
	{
		uint64		v[] = {PG_UINT64_MAX, 3, 0, 5, 0, u64_max_3, 60, 200, 50, 7, 10};

		alloctests_sort_tests_check_is_sorted_U64(v, lengthof(v));
	}
	alloctests_sort_patterns_random(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2],
									&frame->large);
	frame->large_live = true;
	std_vec_push_Checked(&frame->large, &max, sizeof(int32), _Alignof(int32));
	std_vec_push_Checked(&frame->large, &min, sizeof(int32), _Alignof(int32));
	std_vec_push_Checked(&frame->large, &max, sizeof(int32), _Alignof(int32));
	alloctests_sort_tests_check_is_sorted_I32(frame->large.ptr, frame->large.len);
	frame->large_live = false;
	std_vec_free_Checked(&frame->large, sizeof(int32), _Alignof(int32));
}

/* alloctests_sort_tests_int_edge_pad_0 is int_edge's pad: the large pattern, while live. */
static void
alloctests_sort_tests_int_edge_pad_0(void *frame_)
{
	alloctests_sort_tests_int_edge_frame_0_t *frame = frame_;

	if (frame->large_live)
		std_vec_free_Checked(&frame->large, sizeof(int32), _Alignof(int32));
}

/* test_alloctests_sort_tests_stable_int_edge is the test int_edge. */
void
test_alloctests_sort_tests_stable_int_edge(void)
{
	alloctests_sort_tests_int_edge_frame_0_t frame = {0};

	std_landing(&frame, alloctests_sort_tests_int_edge_body_0, alloctests_sort_tests_int_edge_pad_0);
}

/* alloctests_sort_tests_sort_vs_sort_by_impl_closure_0 is sort_vs_sort_by's comparison: a.cmp(b). */
static StdOrdering
alloctests_sort_tests_sort_vs_sort_by_impl_closure_0(void *captures, const void *a, const void *b)
{
	int32		x = *(const int32 *) a;
	int32		y = *(const int32 *) b;

	return x < y ? STD_ORDERING_LESS : (x > y ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* alloctests_sort_tests_sort_vs_sort_by_frame_0_t is the test's state across its pad: the copy it sorts by comparison, while live. */
typedef struct alloctests_sort_tests_sort_vs_sort_by_frame_0_t
{
	StdVec		input_sort_by;
	bool		input_sort_by_live;
} alloctests_sort_tests_sort_vs_sort_by_frame_0_t;

/* alloctests_sort_tests_sort_vs_sort_by_body_0 is the test: sort and sort_by with Ord's cmp give the same order. */
static void
alloctests_sort_tests_sort_vs_sort_by_body_0(void *frame_)
{
	alloctests_sort_tests_sort_vs_sort_by_frame_0_t *frame = frame_;
	int32		input_normal[] = {800, 3, -801, 5, -801, -3, 60, 200, 50, 7, 10};
	int32		expected[] = {-801, -801, -3, 3, 5, 7, 10, 50, 60, 200, 800};

	std_slice_to_vec_Checked(&frame->input_sort_by, input_normal, lengthof(input_normal), sizeof(int32),
							 _Alignof(int32));
	frame->input_sort_by_live = true;
	std_slice_sort_I32(input_normal, lengthof(input_normal));
	alloctests_sort_tests_stable_SortImpl_sort_by_I32(frame->input_sort_by.ptr, frame->input_sort_by.len,
													  alloctests_sort_tests_sort_vs_sort_by_impl_closure_0, NULL,
													  NULL);
	STD_ASSERT_EQ(361, 5, alloctests_sort_tests_Array11I32_PartialEqArray11I32_eq(input_normal, expected),
				  input_normal, alloctests_sort_tests_Array11I32_Debug_fmt, expected,
				  alloctests_sort_tests_Array11I32_Debug_fmt);
	STD_ASSERT_EQ(362, 5, alloctests_sort_tests_VecI32_PartialEqArray11I32_eq(&frame->input_sort_by, expected),
				  &frame->input_sort_by, alloctests_sort_tests_VecI32_Debug_fmt, expected,
				  alloctests_sort_tests_Array11I32_Debug_fmt);
	frame->input_sort_by_live = false;
	std_vec_free_Checked(&frame->input_sort_by, sizeof(int32), _Alignof(int32));
}

/* alloctests_sort_tests_sort_vs_sort_by_pad_0 is the test's pad: the copy, while live. */
static void
alloctests_sort_tests_sort_vs_sort_by_pad_0(void *frame_)
{
	alloctests_sort_tests_sort_vs_sort_by_frame_0_t *frame = frame_;

	if (frame->input_sort_by_live)
		std_vec_free_Checked(&frame->input_sort_by, sizeof(int32), _Alignof(int32));
}

/* test_alloctests_sort_tests_stable_sort_vs_sort_by is the test sort_vs_sort_by. */
void
test_alloctests_sort_tests_stable_sort_vs_sort_by(void)
{
	alloctests_sort_tests_sort_vs_sort_by_frame_0_t frame = {0};

	std_landing(&frame, alloctests_sort_tests_sort_vs_sort_by_body_0, alloctests_sort_tests_sort_vs_sort_by_pad_0);
}

/* alloctests_sort_tests_box_value_impl_closure_0 is box_value's second pattern: random_sorted at 80 percent. */
static void
alloctests_sort_tests_box_value_impl_closure_0(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 80.0, result);
}

/* test_alloctests_sort_tests_stable_box_value is the test box_value: boxed values from two patterns at five lengths. */
void
test_alloctests_sort_tests_stable_box_value(void)
{
	static const size_t lens[] = {3, 9, 35, 56, 132};

	for (size_t k = 0, k_len = lengthof(lens); k < k_len; k++)
	{
		size_t		len = lens[k];

		alloctests_sort_tests_test_is_sorted_BoxI32(len, alloctests_sort_tests_Box_new_into, NULL, NULL,
													alloctests_sort_patterns_random_closure, NULL, NULL);
		alloctests_sort_tests_test_is_sorted_BoxI32(len, alloctests_sort_tests_Box_new_into, NULL, NULL,
													alloctests_sort_tests_box_value_impl_closure_0, NULL, NULL);
	}
}

/* alloctests_sort_tests_VecU8_Debug_fmt is Vec<u8>'s Debug, its slice's. */
static bool
alloctests_sort_tests_VecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdBytes	bytes = {self->ptr, self->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_frame_0_t is i32_tup_as_u64's state across its pad: the concatenated bytes, while live. */
typedef struct alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_frame_0_t
{
	AlloctestsSortTestsTupleI32I32 val;
	StdVec		bytes;
	bool		bytes_live;
	uint64		result;
} alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_frame_0_t;

/* alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_body_0 is i32_tup_as_u64's body: both values' little-endian bytes, concatenated, as one u64. */
static void
alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_body_0(void *frame_)
{
	alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_frame_0_t *frame = frame_;
	uint8		a_bytes[4];
	uint8		b_bytes[4];
	StdBytes	parts[2];
	uint8		array[8];

	std_u32_to_le_bytes((uint32) frame->val.f0, a_bytes);
	std_u32_to_le_bytes((uint32) frame->val.f1, b_bytes);
	parts[0].ptr = a_bytes;
	parts[0].len = sizeof(a_bytes);
	parts[1].ptr = b_bytes;
	parts[1].len = sizeof(b_bytes);
	std_slice_concat(parts, lengthof(parts), sizeof(StdBytes), 1, 1, &frame->bytes);
	frame->bytes_live = true;
	if (frame->bytes.len != sizeof(array))
		STD_RESULT_EXPECT_FAILED(509, 67, "called `Result::unwrap()` on an `Err` value", &frame->bytes,
								 alloctests_sort_tests_VecU8_Debug_fmt);
	frame->bytes.len = 0;
	memcpy(array, frame->bytes.ptr, sizeof(array));
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->result = std_u64_from_le_bytes(array);
}

/* alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_pad_0 is i32_tup_as_u64's pad: the bytes, while live. */
static void
alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_pad_0(void *frame_)
{
	alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_frame_0_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64 is stability_legacy's i32_tup_as_u64: the pair packed into a u64, first value low. */
static uint64
alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64(AlloctestsSortTestsTupleI32I32 val)
{
	alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_frame_0_t frame = {val};

	std_landing(&frame, alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_body_0,
				alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64_pad_0);
	return frame.result;
}

/* alloctests_sort_tests_stability_legacy_impl_i32_tup_from_u64 is stability_legacy's i32_tup_from_u64: the pair a u64 packs. */
static AlloctestsSortTestsTupleI32I32
alloctests_sort_tests_stability_legacy_impl_i32_tup_from_u64(uint64 val)
{
	uint8		bytes[8];
	AlloctestsSortTestsTupleI32I32 result;

	std_u64_to_le_bytes(val, bytes);
	std_range_check(0, 4, sizeof(bytes), RUST_FILE, 515, 41);
	if (4 - 0 != 4)
		STD_RESULT_EXPECT_FAILED(515, 59, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	result.f0 = (int32) std_u32_from_le_bytes(&bytes[0]);
	std_range_check(4, 8, sizeof(bytes), RUST_FILE, 516, 41);
	if (8 - 4 != 4)
		STD_RESULT_EXPECT_FAILED(516, 59, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	result.f1 = (int32) std_u32_from_le_bytes(&bytes[4]);
	return result;
}

/* AlloctestsSortTestsStabilityLegacyImplClosure0 is stability_legacy's map closure's captures: the random values, the index into them, and the counts, by reference. */
typedef struct AlloctestsSortTestsStabilityLegacyImplClosure0
{
	const StdVec *rand_vals;
	size_t	   *rand_idx;
	int32	   *counts;
} AlloctestsSortTestsStabilityLegacyImplClosure0;

/* alloctests_sort_tests_stability_legacy_impl_closure_0 is stability_legacy's map: the next random value, packed with its occurrence. */
static uint64
alloctests_sort_tests_stability_legacy_impl_closure_0(AlloctestsSortTestsStabilityLegacyImplClosure0 *closure)
{
	int32		n;
	AlloctestsSortTestsTupleI32I32 pair;

	std_index_check(*closure->rand_idx, closure->rand_vals->len, RUST_FILE, 474, 38);
	n = STD_VEC_AT(closure->rand_vals, int32, *closure->rand_idx);
	*closure->rand_idx = STD_ADD(usize, *closure->rand_idx, 1, 475, 21);
	if (*closure->rand_idx >= closure->rand_vals->len)
		*closure->rand_idx = 0;
	std_index_check((size_t) n, 10, RUST_FILE, 480, 21);
	closure->counts[(size_t) n] = STD_ADD(i32, closure->counts[(size_t) n], 1, 480, 21);
	std_index_check((size_t) n, 10, RUST_FILE, 481, 40);
	pair.f0 = n;
	pair.f1 = closure->counts[(size_t) n];
	return alloctests_sort_tests_stability_legacy_impl_i32_tup_as_u64(pair);
}

/* alloctests_sort_tests_stability_legacy_impl_closure_1 is stability_legacy's comparison: the packed pairs' first values' cmp. */
static StdOrdering
alloctests_sort_tests_stability_legacy_impl_closure_1(void *captures, const void *a_packed, const void *b_packed)
{
	int32		a = alloctests_sort_tests_stability_legacy_impl_i32_tup_from_u64(*(const uint64 *) a_packed).f0;
	int32		b = alloctests_sort_tests_stability_legacy_impl_i32_tup_from_u64(*(const uint64 *) b_packed).f0;

	return std_i32_Ord_cmp(&a, &b);
}

/* alloctests_sort_tests_stability_legacy_impl_closure_2 is stability_legacy's check of a window: its pairs in order. */
static bool
alloctests_sort_tests_stability_legacy_impl_closure_2(const uint64 *w, size_t w_len)
{
	AlloctestsSortTestsTupleI32I32 x;
	AlloctestsSortTestsTupleI32I32 y;

	std_index_check(0, w_len, RUST_FILE, 500, 59);
	x = alloctests_sort_tests_stability_legacy_impl_i32_tup_from_u64(w[0]);
	std_index_check(1, w_len, RUST_FILE, 500, 85);
	y = alloctests_sort_tests_stability_legacy_impl_i32_tup_from_u64(w[1]);
	return x.f0 != y.f0 ? x.f0 < y.f0 : x.f1 <= y.f1;
}

/* alloctests_sort_tests_stability_legacy_impl_frame_0_t is stability_legacy's state across its pad: Sort::name's String, the random values, and the two vectors, while live. */
typedef struct alloctests_sort_tests_stability_legacy_impl_frame_0_t
{
	StdString	name;
	bool		name_live;
	StdVec		rand_vals;
	bool		rand_vals_live;
	StdVec		orig;
	bool		orig_live;
	StdVec		v;
	bool		v_live;
} alloctests_sort_tests_stability_legacy_impl_frame_0_t;

/* alloctests_sort_tests_stability_legacy_impl_body_0 is stability_legacy's body: random keys tagged with their occurrence, sorted by key, are in tag order. */
static void
alloctests_sort_tests_stability_legacy_impl_body_0(void *frame_)
{
	alloctests_sort_tests_stability_legacy_impl_frame_0_t *frame = frame_;
	static const char unstable[] = "unstable";
	size_t		large_range_start = 3000;
	size_t		large_range_end = 3010;
	size_t		rounds = 10;
	size_t		rand_idx = 0;
	bool		skip;

	alloctests_sort_tests_stable_SortImpl_name(&frame->name);
	frame->name_live = true;
	skip = std_str_contains_str_Checked(frame->name.ptr, frame->name.len, unstable, sizeof(unstable) - 1);
	frame->name_live = false;
	std_String_drop(&frame->name);
	if (skip)
		return;
	alloctests_sort_patterns_random_uniform_RangeInclusiveI32(5000, 0, 9, &frame->rand_vals);
	frame->rand_vals_live = true;
	for (int chain = 0; chain < 2; chain++)
	{
		size_t		start = chain == 0 ? 2 : large_range_start;
		size_t		end = chain == 0 ? 55 : large_range_end;

		for (size_t len = start; len < end; len = std_forward_unchecked_usize_Checked(len, 1))
		{
			for (size_t round = 0; round < rounds; round = std_forward_unchecked_usize_Checked(round, 1))
			{
				int32		counts[10] = {0};
				AlloctestsSortTestsStabilityLegacyImplClosure0 closure = {&frame->rand_vals, &rand_idx, counts};
				StdSizeHint hint = {len, true, len};
				StdWindows	windows;
				const void *window;
				size_t		window_len;
				bool		all = true;

				std_vec_with_capacity_trusted(&frame->orig, hint, sizeof(uint64), _Alignof(uint64));
				frame->orig_live = true;
				std_vec_extend_trusted_reserve(&frame->orig, hint, sizeof(uint64), _Alignof(uint64));
				for (size_t k = 0; k < len; k = std_forward_unchecked_usize_Checked(k, 1))
				{
					uint64		packed = alloctests_sort_tests_stability_legacy_impl_closure_0(&closure);

					STD_VEC_AT(&frame->orig, uint64, frame->orig.len) = packed;
					frame->orig.len = std_wrapping_add_usize(frame->orig.len, 1);
				}
				std_vec_clone(&frame->v, &frame->orig, sizeof(uint64), _Alignof(uint64));
				frame->v_live = true;
				alloctests_sort_tests_stable_SortImpl_sort_by_U64(frame->v.ptr, frame->v.len,
																  alloctests_sort_tests_stability_legacy_impl_closure_1,
																  NULL, NULL);
				windows = std_slice_windows(frame->v.ptr, frame->v.len, 2, RUST_FILE, 500, 23);
				while (std_slice_Windows_next(&windows, sizeof(uint64), &window, &window_len))
				{
					if (!alloctests_sort_tests_stability_legacy_impl_closure_2(window, window_len))
					{
						all = false;
						break;
					}
				}
				STD_ASSERT(500, 13, all,
						   "v.windows(2).all(|w| i32_tup_from_u64(w[0]) <= i32_tup_from_u64(w[1]))");
				frame->v_live = false;
				std_vec_free_Checked(&frame->v, sizeof(uint64), _Alignof(uint64));
				frame->orig_live = false;
				std_vec_free_Checked(&frame->orig, sizeof(uint64), _Alignof(uint64));
			}
		}
	}
	frame->rand_vals_live = false;
	std_vec_free_Checked(&frame->rand_vals, sizeof(int32), _Alignof(int32));
}

/* alloctests_sort_tests_stability_legacy_impl_pad_0 is stability_legacy's pad: its String and vectors, while live. */
static void
alloctests_sort_tests_stability_legacy_impl_pad_0(void *frame_)
{
	alloctests_sort_tests_stability_legacy_impl_frame_0_t *frame = frame_;

	if (frame->v_live)
		std_vec_free_Checked(&frame->v, sizeof(uint64), _Alignof(uint64));
	if (frame->orig_live)
		std_vec_free_Checked(&frame->orig, sizeof(uint64), _Alignof(uint64));
	if (frame->rand_vals_live)
		std_vec_free_Checked(&frame->rand_vals, sizeof(int32), _Alignof(int32));
	if (frame->name_live)
		std_String_drop(&frame->name);
}

/* test_alloctests_sort_tests_stable_stability_legacy is the test stability_legacy. */
void
test_alloctests_sort_tests_stable_stability_legacy(void)
{
	alloctests_sort_tests_stability_legacy_impl_frame_0_t frame = {0};

	std_landing(&frame, alloctests_sort_tests_stability_legacy_impl_body_0,
				alloctests_sort_tests_stability_legacy_impl_pad_0);
}

/* alloctests_sort_tests_correct_i32_random_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_impl_closure_0 is alloctests_sort_tests_correct_i32_random_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random is the test correct_i32_random. */
void
test_alloctests_sort_tests_stable_correct_i32_random(void)
{
	alloctests_sort_tests_correct_i32_random_impl();
}

/* alloctests_sort_tests_correct_i32_random_z1_impl_closure_1 is alloctests_sort_tests_correct_i32_random_z1_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_z1_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_correct_i32_random_z1_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_z1_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_z1_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_z1_impl_closure_0 is alloctests_sort_tests_correct_i32_random_z1_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_z1_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_z1_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_z1_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_z1_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_z1 is the test correct_i32_random_z1. */
void
test_alloctests_sort_tests_stable_correct_i32_random_z1(void)
{
	alloctests_sort_tests_correct_i32_random_z1_impl();
}

/* alloctests_sort_tests_correct_i32_random_d2_impl_closure_1 is alloctests_sort_tests_correct_i32_random_d2_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_d2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_correct_i32_random_d2_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_d2_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_d2_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_d2_impl_closure_0 is alloctests_sort_tests_correct_i32_random_d2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_d2_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_d2_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_d2_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_d2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_d2 is the test correct_i32_random_d2. */
void
test_alloctests_sort_tests_stable_correct_i32_random_d2(void)
{
	alloctests_sort_tests_correct_i32_random_d2_impl();
}

/* alloctests_sort_tests_correct_i32_random_d20_impl_closure_1 is alloctests_sort_tests_correct_i32_random_d20_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_d20_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_correct_i32_random_d20_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_d20_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_d20_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_d20_impl_closure_0 is alloctests_sort_tests_correct_i32_random_d20_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_d20_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_d20_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_d20_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_d20_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_d20 is the test correct_i32_random_d20. */
void
test_alloctests_sort_tests_stable_correct_i32_random_d20(void)
{
	alloctests_sort_tests_correct_i32_random_d20_impl();
}

/* alloctests_sort_tests_correct_i32_random_s95_impl_closure_1 is alloctests_sort_tests_correct_i32_random_s95_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_s95_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_correct_i32_random_s95_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_s95_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_s95_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_s95_impl_closure_0 is alloctests_sort_tests_correct_i32_random_s95_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_s95_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_s95_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_s95_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_s95_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_s95 is the test correct_i32_random_s95. */
void
test_alloctests_sort_tests_stable_correct_i32_random_s95(void)
{
	alloctests_sort_tests_correct_i32_random_s95_impl();
}

/* alloctests_sort_tests_correct_i32_ascending_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_ascending_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_ascending_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_ascending_impl_closure_0 is alloctests_sort_tests_correct_i32_ascending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_ascending_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_ascending_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_ascending_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_ascending is the test correct_i32_ascending. */
void
test_alloctests_sort_tests_stable_correct_i32_ascending(void)
{
	alloctests_sort_tests_correct_i32_ascending_impl();
}

/* alloctests_sort_tests_correct_i32_descending_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_descending_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_descending_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_descending_impl_closure_0 is alloctests_sort_tests_correct_i32_descending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_descending_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_descending_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_descending_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_descending is the test correct_i32_descending. */
void
test_alloctests_sort_tests_stable_correct_i32_descending(void)
{
	alloctests_sort_tests_correct_i32_descending_impl();
}

/* alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_correct_i32_saw_mixed_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_saw_mixed_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_correct_i32_saw_mixed_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_saw_mixed_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_saw_mixed_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_saw_mixed is the test correct_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_correct_i32_saw_mixed(void)
{
	alloctests_sort_tests_correct_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_correct_i32_random_d4_impl_closure_1 is alloctests_sort_tests_correct_i32_random_d4_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_d4_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 4, result);
}

/* alloctests_sort_tests_correct_i32_random_d4_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_d4_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_d4_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_d4_impl_closure_0 is alloctests_sort_tests_correct_i32_random_d4_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_d4_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_d4_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_d4_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_d4_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_d4_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_d4_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_d4 is the test correct_i32_random_d4. */
void
test_alloctests_sort_tests_stable_correct_i32_random_d4(void)
{
	alloctests_sort_tests_correct_i32_random_d4_impl();
}

/* alloctests_sort_tests_correct_i32_random_d8_impl_closure_1 is alloctests_sort_tests_correct_i32_random_d8_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_d8_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 8, result);
}

/* alloctests_sort_tests_correct_i32_random_d8_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_d8_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_d8_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_d8_impl_closure_0 is alloctests_sort_tests_correct_i32_random_d8_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_d8_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_d8_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_d8_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_d8_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_d8_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_d8_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_d8 is the test correct_i32_random_d8. */
void
test_alloctests_sort_tests_stable_correct_i32_random_d8(void)
{
	alloctests_sort_tests_correct_i32_random_d8_impl();
}

/* alloctests_sort_tests_correct_i32_random_d311_impl_closure_1 is alloctests_sort_tests_correct_i32_random_d311_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_d311_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 311, result);
}

/* alloctests_sort_tests_correct_i32_random_d311_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_d311_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_d311_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_d311_impl_closure_0 is alloctests_sort_tests_correct_i32_random_d311_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_d311_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_d311_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_d311_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_d311_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_d311_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_d311_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_d311 is the test correct_i32_random_d311. */
void
test_alloctests_sort_tests_stable_correct_i32_random_d311(void)
{
	alloctests_sort_tests_correct_i32_random_d311_impl();
}

/* alloctests_sort_tests_correct_i32_random_d1024_impl_closure_1 is alloctests_sort_tests_correct_i32_random_d1024_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_d1024_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 1024, result);
}

/* alloctests_sort_tests_correct_i32_random_d1024_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_d1024_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_d1024_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_d1024_impl_closure_0 is alloctests_sort_tests_correct_i32_random_d1024_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_d1024_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_d1024_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_d1024_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_d1024_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_d1024_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_d1024_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_d1024 is the test correct_i32_random_d1024. */
void
test_alloctests_sort_tests_stable_correct_i32_random_d1024(void)
{
	alloctests_sort_tests_correct_i32_random_d1024_impl();
}

/* alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_1 is alloctests_sort_tests_correct_i32_random_z1_03_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.03, result);
}

/* alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_z1_03_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_0 is alloctests_sort_tests_correct_i32_random_z1_03_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_z1_03_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_z1_03_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_z1_03_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_z1_03 is the test correct_i32_random_z1_03. */
void
test_alloctests_sort_tests_stable_correct_i32_random_z1_03(void)
{
	alloctests_sort_tests_correct_i32_random_z1_03_impl();
}

/* alloctests_sort_tests_correct_i32_random_z2_impl_closure_1 is alloctests_sort_tests_correct_i32_random_z2_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_z2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 2.0, result);
}

/* alloctests_sort_tests_correct_i32_random_z2_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_z2_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_z2_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_z2_impl_closure_0 is alloctests_sort_tests_correct_i32_random_z2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_z2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_z2_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_z2_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_z2_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_z2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_z2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_z2 is the test correct_i32_random_z2. */
void
test_alloctests_sort_tests_stable_correct_i32_random_z2(void)
{
	alloctests_sort_tests_correct_i32_random_z2_impl();
}

/* alloctests_sort_tests_correct_i32_random_s50_impl_closure_1 is alloctests_sort_tests_correct_i32_random_s50_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_random_s50_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 50.0, result);
}

/* alloctests_sort_tests_correct_i32_random_s50_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_random_s50_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_random_s50_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_random_s50_impl_closure_0 is alloctests_sort_tests_correct_i32_random_s50_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_random_s50_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_random_s50_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_random_s50_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_random_s50_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_random_s50_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_random_s50_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_random_s50 is the test correct_i32_random_s50. */
void
test_alloctests_sort_tests_stable_correct_i32_random_s50(void)
{
	alloctests_sort_tests_correct_i32_random_s50_impl();
}

/* alloctests_sort_tests_correct_i32_narrow_impl_closure_1 is alloctests_sort_tests_correct_i32_narrow_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_narrow_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeInclusiveI32(len, 0, std_mul_i32(std_as_f64_i32(std_f64_round(std_f64_log2((float8) len))), 100, RUST_FILE, 386, 17), result);
}

/* alloctests_sort_tests_correct_i32_narrow_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_narrow_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_narrow_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_narrow_impl_closure_0 is alloctests_sort_tests_correct_i32_narrow_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_narrow_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_narrow_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_narrow_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_narrow_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_narrow_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_narrow_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_narrow is the test correct_i32_narrow. */
void
test_alloctests_sort_tests_stable_correct_i32_narrow(void)
{
	alloctests_sort_tests_correct_i32_narrow_impl();
}

/* alloctests_sort_tests_correct_i32_all_equal_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_all_equal_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_all_equal_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_all_equal_impl_closure_0 is alloctests_sort_tests_correct_i32_all_equal_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_all_equal_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_all_equal_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_all_equal_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_all_equal_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_all_equal_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_all_equal_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_all_equal is the test correct_i32_all_equal. */
void
test_alloctests_sort_tests_stable_correct_i32_all_equal(void)
{
	alloctests_sort_tests_correct_i32_all_equal_impl();
}

/* alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_1 is alloctests_sort_tests_correct_i32_saw_mixed_range_impl's pattern. */
static void
alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed_range(len, 20, 50, result);
}

/* alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_saw_mixed_range_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_0 is alloctests_sort_tests_correct_i32_saw_mixed_range_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_saw_mixed_range_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_saw_mixed_range_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_i32_saw_mixed_range_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_saw_mixed_range is the test correct_i32_saw_mixed_range. */
void
test_alloctests_sort_tests_stable_correct_i32_saw_mixed_range(void)
{
	alloctests_sort_tests_correct_i32_saw_mixed_range_impl();
}

/* alloctests_sort_tests_correct_i32_pipe_organ_impl_closure_0_closure_0 is alloctests_sort_tests_correct_i32_pipe_organ_impl's map into the type. */
static void
alloctests_sort_tests_correct_i32_pipe_organ_impl_closure_0_closure_0(void *captures, int32 val, int32 *result)
{
	*result = val;
}

/* alloctests_sort_tests_correct_i32_pipe_organ_impl_closure_0 is alloctests_sort_tests_correct_i32_pipe_organ_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_i32_pipe_organ_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_I32(len, alloctests_sort_tests_correct_i32_pipe_organ_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_i32_pipe_organ_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_i32_pipe_organ_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_i32_pipe_organ_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_pipe_organ_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_i32_pipe_organ is the test correct_i32_pipe_organ. */
void
test_alloctests_sort_tests_stable_correct_i32_pipe_organ(void)
{
	alloctests_sort_tests_correct_i32_pipe_organ_impl();
}

/* alloctests_sort_tests_correct_u64_random_impl_closure_0 is alloctests_sort_tests_correct_u64_random_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u64_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U64(len, alloctests_sort_tests_extend_i32_to_u64_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u64_random_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_u64_random_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_u64_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u64_random is the test correct_u64_random. */
void
test_alloctests_sort_tests_stable_correct_u64_random(void)
{
	alloctests_sort_tests_correct_u64_random_impl();
}

/* alloctests_sort_tests_correct_u64_random_z1_impl_closure_1 is alloctests_sort_tests_correct_u64_random_z1_impl's pattern. */
static void
alloctests_sort_tests_correct_u64_random_z1_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_correct_u64_random_z1_impl_closure_0 is alloctests_sort_tests_correct_u64_random_z1_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u64_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U64(len, alloctests_sort_tests_extend_i32_to_u64_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u64_random_z1_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_u64_random_z1_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_u64_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u64_random_z1_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u64_random_z1 is the test correct_u64_random_z1. */
void
test_alloctests_sort_tests_stable_correct_u64_random_z1(void)
{
	alloctests_sort_tests_correct_u64_random_z1_impl();
}

/* alloctests_sort_tests_correct_u64_random_d2_impl_closure_1 is alloctests_sort_tests_correct_u64_random_d2_impl's pattern. */
static void
alloctests_sort_tests_correct_u64_random_d2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_correct_u64_random_d2_impl_closure_0 is alloctests_sort_tests_correct_u64_random_d2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u64_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U64(len, alloctests_sort_tests_extend_i32_to_u64_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u64_random_d2_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_u64_random_d2_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_u64_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u64_random_d2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u64_random_d2 is the test correct_u64_random_d2. */
void
test_alloctests_sort_tests_stable_correct_u64_random_d2(void)
{
	alloctests_sort_tests_correct_u64_random_d2_impl();
}

/* alloctests_sort_tests_correct_u64_random_d20_impl_closure_1 is alloctests_sort_tests_correct_u64_random_d20_impl's pattern. */
static void
alloctests_sort_tests_correct_u64_random_d20_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_correct_u64_random_d20_impl_closure_0 is alloctests_sort_tests_correct_u64_random_d20_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u64_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U64(len, alloctests_sort_tests_extend_i32_to_u64_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u64_random_d20_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_u64_random_d20_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_u64_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u64_random_d20_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u64_random_d20 is the test correct_u64_random_d20. */
void
test_alloctests_sort_tests_stable_correct_u64_random_d20(void)
{
	alloctests_sort_tests_correct_u64_random_d20_impl();
}

/* alloctests_sort_tests_correct_u64_random_s95_impl_closure_1 is alloctests_sort_tests_correct_u64_random_s95_impl's pattern. */
static void
alloctests_sort_tests_correct_u64_random_s95_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_correct_u64_random_s95_impl_closure_0 is alloctests_sort_tests_correct_u64_random_s95_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u64_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U64(len, alloctests_sort_tests_extend_i32_to_u64_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u64_random_s95_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_u64_random_s95_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_u64_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u64_random_s95_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u64_random_s95 is the test correct_u64_random_s95. */
void
test_alloctests_sort_tests_stable_correct_u64_random_s95(void)
{
	alloctests_sort_tests_correct_u64_random_s95_impl();
}

/* alloctests_sort_tests_correct_u64_ascending_impl_closure_0 is alloctests_sort_tests_correct_u64_ascending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u64_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U64(len, alloctests_sort_tests_extend_i32_to_u64_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u64_ascending_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_u64_ascending_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_u64_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u64_ascending is the test correct_u64_ascending. */
void
test_alloctests_sort_tests_stable_correct_u64_ascending(void)
{
	alloctests_sort_tests_correct_u64_ascending_impl();
}

/* alloctests_sort_tests_correct_u64_descending_impl_closure_0 is alloctests_sort_tests_correct_u64_descending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u64_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U64(len, alloctests_sort_tests_extend_i32_to_u64_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u64_descending_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_u64_descending_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_u64_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u64_descending is the test correct_u64_descending. */
void
test_alloctests_sort_tests_stable_correct_u64_descending(void)
{
	alloctests_sort_tests_correct_u64_descending_impl();
}

/* alloctests_sort_tests_correct_u64_saw_mixed_impl_closure_1 is alloctests_sort_tests_correct_u64_saw_mixed_impl's pattern. */
static void
alloctests_sort_tests_correct_u64_saw_mixed_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_correct_u64_saw_mixed_impl_closure_0 is alloctests_sort_tests_correct_u64_saw_mixed_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u64_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U64(len, alloctests_sort_tests_extend_i32_to_u64_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u64_saw_mixed_impl runs its test at each of TEST_LENGTHS. */
static void
alloctests_sort_tests_correct_u64_saw_mixed_impl(void)
{
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS); k < k_len; k++)
		alloctests_sort_tests_correct_u64_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u64_saw_mixed_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u64_saw_mixed is the test correct_u64_saw_mixed. */
void
test_alloctests_sort_tests_stable_correct_u64_saw_mixed(void)
{
	alloctests_sort_tests_correct_u64_saw_mixed_impl();
}

/* alloctests_sort_tests_correct_u128_random_impl_closure_0 is alloctests_sort_tests_correct_u128_random_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u128_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U128(len, alloctests_sort_tests_extend_i32_to_u128_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u128_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_u128_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 404, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_u128_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u128_random is the test correct_u128_random. */
void
test_alloctests_sort_tests_stable_correct_u128_random(void)
{
	alloctests_sort_tests_correct_u128_random_impl();
}

/* alloctests_sort_tests_correct_u128_random_z1_impl_closure_1 is alloctests_sort_tests_correct_u128_random_z1_impl's pattern. */
static void
alloctests_sort_tests_correct_u128_random_z1_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_correct_u128_random_z1_impl_closure_0 is alloctests_sort_tests_correct_u128_random_z1_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u128_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U128(len, alloctests_sort_tests_extend_i32_to_u128_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u128_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_u128_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 404, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_u128_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u128_random_z1_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u128_random_z1 is the test correct_u128_random_z1. */
void
test_alloctests_sort_tests_stable_correct_u128_random_z1(void)
{
	alloctests_sort_tests_correct_u128_random_z1_impl();
}

/* alloctests_sort_tests_correct_u128_random_d2_impl_closure_1 is alloctests_sort_tests_correct_u128_random_d2_impl's pattern. */
static void
alloctests_sort_tests_correct_u128_random_d2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_correct_u128_random_d2_impl_closure_0 is alloctests_sort_tests_correct_u128_random_d2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u128_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U128(len, alloctests_sort_tests_extend_i32_to_u128_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u128_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_u128_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 404, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_u128_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u128_random_d2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u128_random_d2 is the test correct_u128_random_d2. */
void
test_alloctests_sort_tests_stable_correct_u128_random_d2(void)
{
	alloctests_sort_tests_correct_u128_random_d2_impl();
}

/* alloctests_sort_tests_correct_u128_random_d20_impl_closure_1 is alloctests_sort_tests_correct_u128_random_d20_impl's pattern. */
static void
alloctests_sort_tests_correct_u128_random_d20_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_correct_u128_random_d20_impl_closure_0 is alloctests_sort_tests_correct_u128_random_d20_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u128_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U128(len, alloctests_sort_tests_extend_i32_to_u128_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u128_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_u128_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 404, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_u128_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u128_random_d20_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u128_random_d20 is the test correct_u128_random_d20. */
void
test_alloctests_sort_tests_stable_correct_u128_random_d20(void)
{
	alloctests_sort_tests_correct_u128_random_d20_impl();
}

/* alloctests_sort_tests_correct_u128_random_s95_impl_closure_1 is alloctests_sort_tests_correct_u128_random_s95_impl's pattern. */
static void
alloctests_sort_tests_correct_u128_random_s95_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_correct_u128_random_s95_impl_closure_0 is alloctests_sort_tests_correct_u128_random_s95_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u128_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U128(len, alloctests_sort_tests_extend_i32_to_u128_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u128_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_u128_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 404, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_u128_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u128_random_s95_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u128_random_s95 is the test correct_u128_random_s95. */
void
test_alloctests_sort_tests_stable_correct_u128_random_s95(void)
{
	alloctests_sort_tests_correct_u128_random_s95_impl();
}

/* alloctests_sort_tests_correct_u128_ascending_impl_closure_0 is alloctests_sort_tests_correct_u128_ascending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u128_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U128(len, alloctests_sort_tests_extend_i32_to_u128_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u128_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_u128_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 404, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_u128_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u128_ascending is the test correct_u128_ascending. */
void
test_alloctests_sort_tests_stable_correct_u128_ascending(void)
{
	alloctests_sort_tests_correct_u128_ascending_impl();
}

/* alloctests_sort_tests_correct_u128_descending_impl_closure_0 is alloctests_sort_tests_correct_u128_descending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u128_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U128(len, alloctests_sort_tests_extend_i32_to_u128_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u128_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_u128_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 404, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_u128_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u128_descending is the test correct_u128_descending. */
void
test_alloctests_sort_tests_stable_correct_u128_descending(void)
{
	alloctests_sort_tests_correct_u128_descending_impl();
}

/* alloctests_sort_tests_correct_u128_saw_mixed_impl_closure_1 is alloctests_sort_tests_correct_u128_saw_mixed_impl's pattern. */
static void
alloctests_sort_tests_correct_u128_saw_mixed_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_correct_u128_saw_mixed_impl_closure_0 is alloctests_sort_tests_correct_u128_saw_mixed_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_u128_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_U128(len, alloctests_sort_tests_extend_i32_to_u128_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_u128_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_u128_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 404, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_u128_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_u128_saw_mixed_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_u128_saw_mixed is the test correct_u128_saw_mixed. */
void
test_alloctests_sort_tests_stable_correct_u128_saw_mixed(void)
{
	alloctests_sort_tests_correct_u128_saw_mixed_impl();
}

/* alloctests_sort_tests_correct_cell_i32_random_impl_closure_0 is alloctests_sort_tests_correct_cell_i32_random_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_cell_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_CellI32(len, alloctests_sort_tests_Cell_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
												 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_cell_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_cell_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 411, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_cell_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_cell_i32_random is the test correct_cell_i32_random. */
void
test_alloctests_sort_tests_stable_correct_cell_i32_random(void)
{
	alloctests_sort_tests_correct_cell_i32_random_impl();
}

/* alloctests_sort_tests_correct_cell_i32_random_z1_impl_closure_1 is alloctests_sort_tests_correct_cell_i32_random_z1_impl's pattern. */
static void
alloctests_sort_tests_correct_cell_i32_random_z1_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_correct_cell_i32_random_z1_impl_closure_0 is alloctests_sort_tests_correct_cell_i32_random_z1_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_cell_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_CellI32(len, alloctests_sort_tests_Cell_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
												 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_cell_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_cell_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 411, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_cell_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_cell_i32_random_z1_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_cell_i32_random_z1 is the test correct_cell_i32_random_z1. */
void
test_alloctests_sort_tests_stable_correct_cell_i32_random_z1(void)
{
	alloctests_sort_tests_correct_cell_i32_random_z1_impl();
}

/* alloctests_sort_tests_correct_cell_i32_random_d2_impl_closure_1 is alloctests_sort_tests_correct_cell_i32_random_d2_impl's pattern. */
static void
alloctests_sort_tests_correct_cell_i32_random_d2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_correct_cell_i32_random_d2_impl_closure_0 is alloctests_sort_tests_correct_cell_i32_random_d2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_cell_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_CellI32(len, alloctests_sort_tests_Cell_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
												 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_cell_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_cell_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 411, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_cell_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_cell_i32_random_d2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_cell_i32_random_d2 is the test correct_cell_i32_random_d2. */
void
test_alloctests_sort_tests_stable_correct_cell_i32_random_d2(void)
{
	alloctests_sort_tests_correct_cell_i32_random_d2_impl();
}

/* alloctests_sort_tests_correct_cell_i32_random_d20_impl_closure_1 is alloctests_sort_tests_correct_cell_i32_random_d20_impl's pattern. */
static void
alloctests_sort_tests_correct_cell_i32_random_d20_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_correct_cell_i32_random_d20_impl_closure_0 is alloctests_sort_tests_correct_cell_i32_random_d20_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_cell_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_CellI32(len, alloctests_sort_tests_Cell_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
												 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_cell_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_cell_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 411, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_cell_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_cell_i32_random_d20_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_cell_i32_random_d20 is the test correct_cell_i32_random_d20. */
void
test_alloctests_sort_tests_stable_correct_cell_i32_random_d20(void)
{
	alloctests_sort_tests_correct_cell_i32_random_d20_impl();
}

/* alloctests_sort_tests_correct_cell_i32_random_s95_impl_closure_1 is alloctests_sort_tests_correct_cell_i32_random_s95_impl's pattern. */
static void
alloctests_sort_tests_correct_cell_i32_random_s95_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_correct_cell_i32_random_s95_impl_closure_0 is alloctests_sort_tests_correct_cell_i32_random_s95_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_cell_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_CellI32(len, alloctests_sort_tests_Cell_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
												 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_cell_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_cell_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 411, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_cell_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_cell_i32_random_s95_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_cell_i32_random_s95 is the test correct_cell_i32_random_s95. */
void
test_alloctests_sort_tests_stable_correct_cell_i32_random_s95(void)
{
	alloctests_sort_tests_correct_cell_i32_random_s95_impl();
}

/* alloctests_sort_tests_correct_cell_i32_ascending_impl_closure_0 is alloctests_sort_tests_correct_cell_i32_ascending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_cell_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_CellI32(len, alloctests_sort_tests_Cell_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
												 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_cell_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_cell_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 411, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_cell_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_cell_i32_ascending is the test correct_cell_i32_ascending. */
void
test_alloctests_sort_tests_stable_correct_cell_i32_ascending(void)
{
	alloctests_sort_tests_correct_cell_i32_ascending_impl();
}

/* alloctests_sort_tests_correct_cell_i32_descending_impl_closure_0 is alloctests_sort_tests_correct_cell_i32_descending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_cell_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_CellI32(len, alloctests_sort_tests_Cell_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
												 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_cell_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_cell_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 411, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_cell_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_cell_i32_descending is the test correct_cell_i32_descending. */
void
test_alloctests_sort_tests_stable_correct_cell_i32_descending(void)
{
	alloctests_sort_tests_correct_cell_i32_descending_impl();
}

/* alloctests_sort_tests_correct_cell_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_correct_cell_i32_saw_mixed_impl's pattern. */
static void
alloctests_sort_tests_correct_cell_i32_saw_mixed_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_correct_cell_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_correct_cell_i32_saw_mixed_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_cell_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_CellI32(len, alloctests_sort_tests_Cell_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
												 pattern_fn_drop);
}

/* alloctests_sort_tests_correct_cell_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_cell_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 411, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_cell_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_cell_i32_saw_mixed_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_cell_i32_saw_mixed is the test correct_cell_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_correct_cell_i32_saw_mixed(void)
{
	alloctests_sort_tests_correct_cell_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_correct_string_random_impl_closure_0_closure_0 is alloctests_sort_tests_correct_string_random_impl's map into the type. */
static void
alloctests_sort_tests_correct_string_random_impl_closure_0_closure_0(void *captures, int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_correct_string_random_impl_closure_0 is alloctests_sort_tests_correct_string_random_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_string_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_String(len, alloctests_sort_tests_correct_string_random_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
												pattern_fn_drop);
}

/* alloctests_sort_tests_correct_string_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_string_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 422, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_string_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_string_random is the test correct_string_random. */
void
test_alloctests_sort_tests_stable_correct_string_random(void)
{
	alloctests_sort_tests_correct_string_random_impl();
}

/* alloctests_sort_tests_correct_string_random_z1_impl_closure_1 is alloctests_sort_tests_correct_string_random_z1_impl's pattern. */
static void
alloctests_sort_tests_correct_string_random_z1_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_correct_string_random_z1_impl_closure_0_closure_0 is alloctests_sort_tests_correct_string_random_z1_impl's map into the type. */
static void
alloctests_sort_tests_correct_string_random_z1_impl_closure_0_closure_0(void *captures, int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_correct_string_random_z1_impl_closure_0 is alloctests_sort_tests_correct_string_random_z1_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_string_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_String(len, alloctests_sort_tests_correct_string_random_z1_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
												pattern_fn_drop);
}

/* alloctests_sort_tests_correct_string_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_string_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 422, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_string_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_string_random_z1_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_string_random_z1 is the test correct_string_random_z1. */
void
test_alloctests_sort_tests_stable_correct_string_random_z1(void)
{
	alloctests_sort_tests_correct_string_random_z1_impl();
}

/* alloctests_sort_tests_correct_string_random_d2_impl_closure_1 is alloctests_sort_tests_correct_string_random_d2_impl's pattern. */
static void
alloctests_sort_tests_correct_string_random_d2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_correct_string_random_d2_impl_closure_0_closure_0 is alloctests_sort_tests_correct_string_random_d2_impl's map into the type. */
static void
alloctests_sort_tests_correct_string_random_d2_impl_closure_0_closure_0(void *captures, int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_correct_string_random_d2_impl_closure_0 is alloctests_sort_tests_correct_string_random_d2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_string_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_String(len, alloctests_sort_tests_correct_string_random_d2_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
												pattern_fn_drop);
}

/* alloctests_sort_tests_correct_string_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_string_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 422, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_string_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_string_random_d2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_string_random_d2 is the test correct_string_random_d2. */
void
test_alloctests_sort_tests_stable_correct_string_random_d2(void)
{
	alloctests_sort_tests_correct_string_random_d2_impl();
}

/* alloctests_sort_tests_correct_string_random_d20_impl_closure_1 is alloctests_sort_tests_correct_string_random_d20_impl's pattern. */
static void
alloctests_sort_tests_correct_string_random_d20_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_correct_string_random_d20_impl_closure_0_closure_0 is alloctests_sort_tests_correct_string_random_d20_impl's map into the type. */
static void
alloctests_sort_tests_correct_string_random_d20_impl_closure_0_closure_0(void *captures, int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_correct_string_random_d20_impl_closure_0 is alloctests_sort_tests_correct_string_random_d20_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_string_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_String(len, alloctests_sort_tests_correct_string_random_d20_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
												pattern_fn_drop);
}

/* alloctests_sort_tests_correct_string_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_string_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 422, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_string_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_string_random_d20_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_string_random_d20 is the test correct_string_random_d20. */
void
test_alloctests_sort_tests_stable_correct_string_random_d20(void)
{
	alloctests_sort_tests_correct_string_random_d20_impl();
}

/* alloctests_sort_tests_correct_string_random_s95_impl_closure_1 is alloctests_sort_tests_correct_string_random_s95_impl's pattern. */
static void
alloctests_sort_tests_correct_string_random_s95_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_correct_string_random_s95_impl_closure_0_closure_0 is alloctests_sort_tests_correct_string_random_s95_impl's map into the type. */
static void
alloctests_sort_tests_correct_string_random_s95_impl_closure_0_closure_0(void *captures, int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_correct_string_random_s95_impl_closure_0 is alloctests_sort_tests_correct_string_random_s95_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_string_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_String(len, alloctests_sort_tests_correct_string_random_s95_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
												pattern_fn_drop);
}

/* alloctests_sort_tests_correct_string_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_string_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 422, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_string_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_string_random_s95_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_string_random_s95 is the test correct_string_random_s95. */
void
test_alloctests_sort_tests_stable_correct_string_random_s95(void)
{
	alloctests_sort_tests_correct_string_random_s95_impl();
}

/* alloctests_sort_tests_correct_string_ascending_impl_closure_0_closure_0 is alloctests_sort_tests_correct_string_ascending_impl's map into the type. */
static void
alloctests_sort_tests_correct_string_ascending_impl_closure_0_closure_0(void *captures, int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_correct_string_ascending_impl_closure_0 is alloctests_sort_tests_correct_string_ascending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_string_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_String(len, alloctests_sort_tests_correct_string_ascending_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
												pattern_fn_drop);
}

/* alloctests_sort_tests_correct_string_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_string_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 422, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_string_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_string_ascending is the test correct_string_ascending. */
void
test_alloctests_sort_tests_stable_correct_string_ascending(void)
{
	alloctests_sort_tests_correct_string_ascending_impl();
}

/* alloctests_sort_tests_correct_string_descending_impl_closure_0_closure_0 is alloctests_sort_tests_correct_string_descending_impl's map into the type. */
static void
alloctests_sort_tests_correct_string_descending_impl_closure_0_closure_0(void *captures, int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_correct_string_descending_impl_closure_0 is alloctests_sort_tests_correct_string_descending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_string_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_String(len, alloctests_sort_tests_correct_string_descending_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
												pattern_fn_drop);
}

/* alloctests_sort_tests_correct_string_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_string_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 422, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_string_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_string_descending is the test correct_string_descending. */
void
test_alloctests_sort_tests_stable_correct_string_descending(void)
{
	alloctests_sort_tests_correct_string_descending_impl();
}

/* alloctests_sort_tests_correct_string_saw_mixed_impl_closure_1 is alloctests_sort_tests_correct_string_saw_mixed_impl's pattern. */
static void
alloctests_sort_tests_correct_string_saw_mixed_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_correct_string_saw_mixed_impl_closure_0_closure_0 is alloctests_sort_tests_correct_string_saw_mixed_impl's map into the type. */
static void
alloctests_sort_tests_correct_string_saw_mixed_impl_closure_0_closure_0(void *captures, int32 val, StdString *result)
{
	uint32		shifted = alloctests_sort_tests_shift_i32_to_u32(val);
	static const StdFmtPiece pieces[] = {{NULL, 0, 0, {.sign_aware_zero_pad = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 10}}};
	StdFmtArg	args[] = {{&shifted, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/* alloctests_sort_tests_correct_string_saw_mixed_impl_closure_0 is alloctests_sort_tests_correct_string_saw_mixed_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_string_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_String(len, alloctests_sort_tests_correct_string_saw_mixed_impl_closure_0_closure_0, NULL, NULL, pattern_fn, pattern_fn_captures,
												pattern_fn_drop);
}

/* alloctests_sort_tests_correct_string_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_string_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 422, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_string_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_string_saw_mixed_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_string_saw_mixed is the test correct_string_saw_mixed. */
void
test_alloctests_sort_tests_stable_correct_string_saw_mixed(void)
{
	alloctests_sort_tests_correct_string_saw_mixed_impl();
}

/* alloctests_sort_tests_correct_f128_random_impl_closure_0 is alloctests_sort_tests_correct_f128_random_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_f128_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_F128(len, alloctests_sort_tests_F128_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_f128_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_f128_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 429, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_f128_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_f128_random is the test correct_f128_random. */
void
test_alloctests_sort_tests_stable_correct_f128_random(void)
{
	alloctests_sort_tests_correct_f128_random_impl();
}

/* alloctests_sort_tests_correct_f128_random_z1_impl_closure_1 is alloctests_sort_tests_correct_f128_random_z1_impl's pattern. */
static void
alloctests_sort_tests_correct_f128_random_z1_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_correct_f128_random_z1_impl_closure_0 is alloctests_sort_tests_correct_f128_random_z1_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_f128_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_F128(len, alloctests_sort_tests_F128_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_f128_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_f128_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 429, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_f128_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_f128_random_z1_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_f128_random_z1 is the test correct_f128_random_z1. */
void
test_alloctests_sort_tests_stable_correct_f128_random_z1(void)
{
	alloctests_sort_tests_correct_f128_random_z1_impl();
}

/* alloctests_sort_tests_correct_f128_random_d2_impl_closure_1 is alloctests_sort_tests_correct_f128_random_d2_impl's pattern. */
static void
alloctests_sort_tests_correct_f128_random_d2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_correct_f128_random_d2_impl_closure_0 is alloctests_sort_tests_correct_f128_random_d2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_f128_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_F128(len, alloctests_sort_tests_F128_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_f128_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_f128_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 429, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_f128_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_f128_random_d2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_f128_random_d2 is the test correct_f128_random_d2. */
void
test_alloctests_sort_tests_stable_correct_f128_random_d2(void)
{
	alloctests_sort_tests_correct_f128_random_d2_impl();
}

/* alloctests_sort_tests_correct_f128_random_d20_impl_closure_1 is alloctests_sort_tests_correct_f128_random_d20_impl's pattern. */
static void
alloctests_sort_tests_correct_f128_random_d20_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_correct_f128_random_d20_impl_closure_0 is alloctests_sort_tests_correct_f128_random_d20_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_f128_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_F128(len, alloctests_sort_tests_F128_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_f128_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_f128_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 429, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_f128_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_f128_random_d20_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_f128_random_d20 is the test correct_f128_random_d20. */
void
test_alloctests_sort_tests_stable_correct_f128_random_d20(void)
{
	alloctests_sort_tests_correct_f128_random_d20_impl();
}

/* alloctests_sort_tests_correct_f128_random_s95_impl_closure_1 is alloctests_sort_tests_correct_f128_random_s95_impl's pattern. */
static void
alloctests_sort_tests_correct_f128_random_s95_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_correct_f128_random_s95_impl_closure_0 is alloctests_sort_tests_correct_f128_random_s95_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_f128_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_F128(len, alloctests_sort_tests_F128_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_f128_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_f128_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 429, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_f128_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_f128_random_s95_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_f128_random_s95 is the test correct_f128_random_s95. */
void
test_alloctests_sort_tests_stable_correct_f128_random_s95(void)
{
	alloctests_sort_tests_correct_f128_random_s95_impl();
}

/* alloctests_sort_tests_correct_f128_ascending_impl_closure_0 is alloctests_sort_tests_correct_f128_ascending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_f128_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_F128(len, alloctests_sort_tests_F128_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_f128_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_f128_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 429, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_f128_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_f128_ascending is the test correct_f128_ascending. */
void
test_alloctests_sort_tests_stable_correct_f128_ascending(void)
{
	alloctests_sort_tests_correct_f128_ascending_impl();
}

/* alloctests_sort_tests_correct_f128_descending_impl_closure_0 is alloctests_sort_tests_correct_f128_descending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_f128_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															 void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_F128(len, alloctests_sort_tests_F128_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_f128_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_f128_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 429, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_f128_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_f128_descending is the test correct_f128_descending. */
void
test_alloctests_sort_tests_stable_correct_f128_descending(void)
{
	alloctests_sort_tests_correct_f128_descending_impl();
}

/* alloctests_sort_tests_correct_f128_saw_mixed_impl_closure_1 is alloctests_sort_tests_correct_f128_saw_mixed_impl's pattern. */
static void
alloctests_sort_tests_correct_f128_saw_mixed_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_correct_f128_saw_mixed_impl_closure_0 is alloctests_sort_tests_correct_f128_saw_mixed_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_f128_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_F128(len, alloctests_sort_tests_F128_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
											  pattern_fn_drop);
}

/* alloctests_sort_tests_correct_f128_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_f128_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 429, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_f128_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_f128_saw_mixed_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_f128_saw_mixed is the test correct_f128_saw_mixed. */
void
test_alloctests_sort_tests_stable_correct_f128_saw_mixed(void)
{
	alloctests_sort_tests_correct_f128_saw_mixed_impl();
}

/* alloctests_sort_tests_correct_1k_random_impl_closure_0 is alloctests_sort_tests_correct_1k_random_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_1k_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
													   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_FFIOneKibiByte(len, alloctests_sort_tests_FFIOneKibiByte_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
														pattern_fn_drop);
}

/* alloctests_sort_tests_correct_1k_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_1k_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 436, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_1k_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_1k_random is the test correct_1k_random. */
void
test_alloctests_sort_tests_stable_correct_1k_random(void)
{
	alloctests_sort_tests_correct_1k_random_impl();
}

/* alloctests_sort_tests_correct_1k_random_z1_impl_closure_1 is alloctests_sort_tests_correct_1k_random_z1_impl's pattern. */
static void
alloctests_sort_tests_correct_1k_random_z1_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_correct_1k_random_z1_impl_closure_0 is alloctests_sort_tests_correct_1k_random_z1_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_1k_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_FFIOneKibiByte(len, alloctests_sort_tests_FFIOneKibiByte_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
														pattern_fn_drop);
}

/* alloctests_sort_tests_correct_1k_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_1k_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 436, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_1k_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_1k_random_z1_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_1k_random_z1 is the test correct_1k_random_z1. */
void
test_alloctests_sort_tests_stable_correct_1k_random_z1(void)
{
	alloctests_sort_tests_correct_1k_random_z1_impl();
}

/* alloctests_sort_tests_correct_1k_random_d2_impl_closure_1 is alloctests_sort_tests_correct_1k_random_d2_impl's pattern. */
static void
alloctests_sort_tests_correct_1k_random_d2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_correct_1k_random_d2_impl_closure_0 is alloctests_sort_tests_correct_1k_random_d2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_1k_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_FFIOneKibiByte(len, alloctests_sort_tests_FFIOneKibiByte_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
														pattern_fn_drop);
}

/* alloctests_sort_tests_correct_1k_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_1k_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 436, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_1k_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_1k_random_d2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_1k_random_d2 is the test correct_1k_random_d2. */
void
test_alloctests_sort_tests_stable_correct_1k_random_d2(void)
{
	alloctests_sort_tests_correct_1k_random_d2_impl();
}

/* alloctests_sort_tests_correct_1k_random_d20_impl_closure_1 is alloctests_sort_tests_correct_1k_random_d20_impl's pattern. */
static void
alloctests_sort_tests_correct_1k_random_d20_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_correct_1k_random_d20_impl_closure_0 is alloctests_sort_tests_correct_1k_random_d20_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_1k_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_FFIOneKibiByte(len, alloctests_sort_tests_FFIOneKibiByte_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
														pattern_fn_drop);
}

/* alloctests_sort_tests_correct_1k_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_1k_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 436, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_1k_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_1k_random_d20_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_1k_random_d20 is the test correct_1k_random_d20. */
void
test_alloctests_sort_tests_stable_correct_1k_random_d20(void)
{
	alloctests_sort_tests_correct_1k_random_d20_impl();
}

/* alloctests_sort_tests_correct_1k_random_s95_impl_closure_1 is alloctests_sort_tests_correct_1k_random_s95_impl's pattern. */
static void
alloctests_sort_tests_correct_1k_random_s95_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_correct_1k_random_s95_impl_closure_0 is alloctests_sort_tests_correct_1k_random_s95_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_1k_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_FFIOneKibiByte(len, alloctests_sort_tests_FFIOneKibiByte_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
														pattern_fn_drop);
}

/* alloctests_sort_tests_correct_1k_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_1k_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 436, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_1k_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_1k_random_s95_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_1k_random_s95 is the test correct_1k_random_s95. */
void
test_alloctests_sort_tests_stable_correct_1k_random_s95(void)
{
	alloctests_sort_tests_correct_1k_random_s95_impl();
}

/* alloctests_sort_tests_correct_1k_ascending_impl_closure_0 is alloctests_sort_tests_correct_1k_ascending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_1k_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_FFIOneKibiByte(len, alloctests_sort_tests_FFIOneKibiByte_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
														pattern_fn_drop);
}

/* alloctests_sort_tests_correct_1k_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_1k_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 436, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_1k_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_1k_ascending is the test correct_1k_ascending. */
void
test_alloctests_sort_tests_stable_correct_1k_ascending(void)
{
	alloctests_sort_tests_correct_1k_ascending_impl();
}

/* alloctests_sort_tests_correct_1k_descending_impl_closure_0 is alloctests_sort_tests_correct_1k_descending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_1k_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_FFIOneKibiByte(len, alloctests_sort_tests_FFIOneKibiByte_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
														pattern_fn_drop);
}

/* alloctests_sort_tests_correct_1k_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_1k_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 436, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_1k_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_1k_descending is the test correct_1k_descending. */
void
test_alloctests_sort_tests_stable_correct_1k_descending(void)
{
	alloctests_sort_tests_correct_1k_descending_impl();
}

/* alloctests_sort_tests_correct_1k_saw_mixed_impl_closure_1 is alloctests_sort_tests_correct_1k_saw_mixed_impl's pattern. */
static void
alloctests_sort_tests_correct_1k_saw_mixed_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_correct_1k_saw_mixed_impl_closure_0 is alloctests_sort_tests_correct_1k_saw_mixed_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_1k_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
														  void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_FFIOneKibiByte(len, alloctests_sort_tests_FFIOneKibiByte_new_into, NULL, NULL, pattern_fn, pattern_fn_captures,
														pattern_fn_drop);
}

/* alloctests_sort_tests_correct_1k_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_1k_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 436, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_1k_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_1k_saw_mixed_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_1k_saw_mixed is the test correct_1k_saw_mixed. */
void
test_alloctests_sort_tests_stable_correct_1k_saw_mixed(void)
{
	alloctests_sort_tests_correct_1k_saw_mixed_impl();
}

/* alloctests_sort_tests_correct_dyn_val_random_impl_closure_0 is alloctests_sort_tests_correct_dyn_val_random_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_dyn_val_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_RcDynTrait(len, alloctests_sort_tests_dyn_trait_from_i32_into, NULL, NULL, pattern_fn, pattern_fn_captures,
													pattern_fn_drop);
}

/* alloctests_sort_tests_correct_dyn_val_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_dyn_val_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 444, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_dyn_val_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_dyn_val_random is the test correct_dyn_val_random. */
void
test_alloctests_sort_tests_stable_correct_dyn_val_random(void)
{
	alloctests_sort_tests_correct_dyn_val_random_impl();
}

/* alloctests_sort_tests_correct_dyn_val_random_z1_impl_closure_1 is alloctests_sort_tests_correct_dyn_val_random_z1_impl's pattern. */
static void
alloctests_sort_tests_correct_dyn_val_random_z1_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_correct_dyn_val_random_z1_impl_closure_0 is alloctests_sort_tests_correct_dyn_val_random_z1_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_dyn_val_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_RcDynTrait(len, alloctests_sort_tests_dyn_trait_from_i32_into, NULL, NULL, pattern_fn, pattern_fn_captures,
													pattern_fn_drop);
}

/* alloctests_sort_tests_correct_dyn_val_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_dyn_val_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 444, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_dyn_val_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_dyn_val_random_z1_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_dyn_val_random_z1 is the test correct_dyn_val_random_z1. */
void
test_alloctests_sort_tests_stable_correct_dyn_val_random_z1(void)
{
	alloctests_sort_tests_correct_dyn_val_random_z1_impl();
}

/* alloctests_sort_tests_correct_dyn_val_random_d2_impl_closure_1 is alloctests_sort_tests_correct_dyn_val_random_d2_impl's pattern. */
static void
alloctests_sort_tests_correct_dyn_val_random_d2_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_correct_dyn_val_random_d2_impl_closure_0 is alloctests_sort_tests_correct_dyn_val_random_d2_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_dyn_val_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_RcDynTrait(len, alloctests_sort_tests_dyn_trait_from_i32_into, NULL, NULL, pattern_fn, pattern_fn_captures,
													pattern_fn_drop);
}

/* alloctests_sort_tests_correct_dyn_val_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_dyn_val_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 444, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_dyn_val_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_dyn_val_random_d2_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_dyn_val_random_d2 is the test correct_dyn_val_random_d2. */
void
test_alloctests_sort_tests_stable_correct_dyn_val_random_d2(void)
{
	alloctests_sort_tests_correct_dyn_val_random_d2_impl();
}

/* alloctests_sort_tests_correct_dyn_val_random_d20_impl_closure_1 is alloctests_sort_tests_correct_dyn_val_random_d20_impl's pattern. */
static void
alloctests_sort_tests_correct_dyn_val_random_d20_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_correct_dyn_val_random_d20_impl_closure_0 is alloctests_sort_tests_correct_dyn_val_random_d20_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_dyn_val_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_RcDynTrait(len, alloctests_sort_tests_dyn_trait_from_i32_into, NULL, NULL, pattern_fn, pattern_fn_captures,
													pattern_fn_drop);
}

/* alloctests_sort_tests_correct_dyn_val_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_dyn_val_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 444, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_dyn_val_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_dyn_val_random_d20_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_dyn_val_random_d20 is the test correct_dyn_val_random_d20. */
void
test_alloctests_sort_tests_stable_correct_dyn_val_random_d20(void)
{
	alloctests_sort_tests_correct_dyn_val_random_d20_impl();
}

/* alloctests_sort_tests_correct_dyn_val_random_s95_impl_closure_1 is alloctests_sort_tests_correct_dyn_val_random_s95_impl's pattern. */
static void
alloctests_sort_tests_correct_dyn_val_random_s95_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_correct_dyn_val_random_s95_impl_closure_0 is alloctests_sort_tests_correct_dyn_val_random_s95_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_dyn_val_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_RcDynTrait(len, alloctests_sort_tests_dyn_trait_from_i32_into, NULL, NULL, pattern_fn, pattern_fn_captures,
													pattern_fn_drop);
}

/* alloctests_sort_tests_correct_dyn_val_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_dyn_val_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 444, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_dyn_val_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_dyn_val_random_s95_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_dyn_val_random_s95 is the test correct_dyn_val_random_s95. */
void
test_alloctests_sort_tests_stable_correct_dyn_val_random_s95(void)
{
	alloctests_sort_tests_correct_dyn_val_random_s95_impl();
}

/* alloctests_sort_tests_correct_dyn_val_ascending_impl_closure_0 is alloctests_sort_tests_correct_dyn_val_ascending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_dyn_val_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_RcDynTrait(len, alloctests_sort_tests_dyn_trait_from_i32_into, NULL, NULL, pattern_fn, pattern_fn_captures,
													pattern_fn_drop);
}

/* alloctests_sort_tests_correct_dyn_val_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_dyn_val_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 444, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_dyn_val_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_dyn_val_ascending is the test correct_dyn_val_ascending. */
void
test_alloctests_sort_tests_stable_correct_dyn_val_ascending(void)
{
	alloctests_sort_tests_correct_dyn_val_ascending_impl();
}

/* alloctests_sort_tests_correct_dyn_val_descending_impl_closure_0 is alloctests_sort_tests_correct_dyn_val_descending_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_dyn_val_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
																void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_RcDynTrait(len, alloctests_sort_tests_dyn_trait_from_i32_into, NULL, NULL, pattern_fn, pattern_fn_captures,
													pattern_fn_drop);
}

/* alloctests_sort_tests_correct_dyn_val_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_dyn_val_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 444, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_dyn_val_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending_closure, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_dyn_val_descending is the test correct_dyn_val_descending. */
void
test_alloctests_sort_tests_stable_correct_dyn_val_descending(void)
{
	alloctests_sort_tests_correct_dyn_val_descending_impl();
}

/* alloctests_sort_tests_correct_dyn_val_saw_mixed_impl_closure_1 is alloctests_sort_tests_correct_dyn_val_saw_mixed_impl's pattern. */
static void
alloctests_sort_tests_correct_dyn_val_saw_mixed_impl_closure_1(void *captures, size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_correct_dyn_val_saw_mixed_impl_closure_0 is alloctests_sort_tests_correct_dyn_val_saw_mixed_impl's test, test_is_sorted at the type. */
static void
alloctests_sort_tests_correct_dyn_val_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (void *captures, size_t len, StdVec *result),
															   void *pattern_fn_captures, void (*pattern_fn_drop) (void *))
{
	alloctests_sort_tests_test_is_sorted_RcDynTrait(len, alloctests_sort_tests_dyn_trait_from_i32_into, NULL, NULL, pattern_fn, pattern_fn_captures,
													pattern_fn_drop);
}

/* alloctests_sort_tests_correct_dyn_val_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_correct_dyn_val_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 444, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_correct_dyn_val_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_correct_dyn_val_saw_mixed_impl_closure_1, NULL, NULL);
}

/* test_alloctests_sort_tests_stable_correct_dyn_val_saw_mixed is the test correct_dyn_val_saw_mixed. */
void
test_alloctests_sort_tests_stable_correct_dyn_val_saw_mixed(void)
{
	alloctests_sort_tests_correct_dyn_val_saw_mixed_impl();
}

/* alloctests_sort_tests_stability_i32_random_impl_closure_0 is alloctests_sort_tests_stability_i32_random_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_stability_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_stability_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_stability_i32_random is the test stability_i32_random. */
void
test_alloctests_sort_tests_stable_stability_i32_random(void)
{
	alloctests_sort_tests_stability_i32_random_impl();
}

/* alloctests_sort_tests_stability_i32_random_z1_impl_closure_1 is alloctests_sort_tests_stability_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_stability_i32_random_z1_impl_closure_0 is alloctests_sort_tests_stability_i32_random_z1_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_stability_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_stability_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_i32_random_z1 is the test stability_i32_random_z1. */
void
test_alloctests_sort_tests_stable_stability_i32_random_z1(void)
{
	alloctests_sort_tests_stability_i32_random_z1_impl();
}

/* alloctests_sort_tests_stability_i32_random_d2_impl_closure_1 is alloctests_sort_tests_stability_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_stability_i32_random_d2_impl_closure_0 is alloctests_sort_tests_stability_i32_random_d2_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_stability_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_stability_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_i32_random_d2 is the test stability_i32_random_d2. */
void
test_alloctests_sort_tests_stable_stability_i32_random_d2(void)
{
	alloctests_sort_tests_stability_i32_random_d2_impl();
}

/* alloctests_sort_tests_stability_i32_random_d20_impl_closure_1 is alloctests_sort_tests_stability_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_stability_i32_random_d20_impl_closure_0 is alloctests_sort_tests_stability_i32_random_d20_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_stability_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_stability_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_i32_random_d20 is the test stability_i32_random_d20. */
void
test_alloctests_sort_tests_stable_stability_i32_random_d20(void)
{
	alloctests_sort_tests_stability_i32_random_d20_impl();
}

/* alloctests_sort_tests_stability_i32_random_s95_impl_closure_1 is alloctests_sort_tests_stability_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_stability_i32_random_s95_impl_closure_0 is alloctests_sort_tests_stability_i32_random_s95_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_stability_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_stability_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_i32_random_s95 is the test stability_i32_random_s95. */
void
test_alloctests_sort_tests_stable_stability_i32_random_s95(void)
{
	alloctests_sort_tests_stability_i32_random_s95_impl();
}

/* alloctests_sort_tests_stability_i32_ascending_impl_closure_0 is alloctests_sort_tests_stability_i32_ascending_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_stability_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_stability_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_stability_i32_ascending is the test stability_i32_ascending. */
void
test_alloctests_sort_tests_stable_stability_i32_ascending(void)
{
	alloctests_sort_tests_stability_i32_ascending_impl();
}

/* alloctests_sort_tests_stability_i32_descending_impl_closure_0 is alloctests_sort_tests_stability_i32_descending_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_stability_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_stability_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_stability_i32_descending is the test stability_i32_descending. */
void
test_alloctests_sort_tests_stable_stability_i32_descending(void)
{
	alloctests_sort_tests_stability_i32_descending_impl();
}

/* alloctests_sort_tests_stability_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_stability_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_stability_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_stability_i32_saw_mixed_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_stability_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_stability_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_i32_saw_mixed is the test stability_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_stability_i32_saw_mixed(void)
{
	alloctests_sort_tests_stability_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_stability_cell_i32_random_impl_closure_0 is alloctests_sort_tests_stability_cell_i32_random_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_cell_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_cell_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_cell_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_cell_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_stability_cell_i32_random is the test stability_cell_i32_random. */
void
test_alloctests_sort_tests_stable_stability_cell_i32_random(void)
{
	alloctests_sort_tests_stability_cell_i32_random_impl();
}

/* alloctests_sort_tests_stability_cell_i32_random_z1_impl_closure_1 is alloctests_sort_tests_stability_cell_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_cell_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_stability_cell_i32_random_z1_impl_closure_0 is alloctests_sort_tests_stability_cell_i32_random_z1_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_cell_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_cell_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_cell_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_cell_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_cell_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_cell_i32_random_z1 is the test stability_cell_i32_random_z1. */
void
test_alloctests_sort_tests_stable_stability_cell_i32_random_z1(void)
{
	alloctests_sort_tests_stability_cell_i32_random_z1_impl();
}

/* alloctests_sort_tests_stability_cell_i32_random_d2_impl_closure_1 is alloctests_sort_tests_stability_cell_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_cell_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_stability_cell_i32_random_d2_impl_closure_0 is alloctests_sort_tests_stability_cell_i32_random_d2_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_cell_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_cell_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_cell_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_cell_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_cell_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_cell_i32_random_d2 is the test stability_cell_i32_random_d2. */
void
test_alloctests_sort_tests_stable_stability_cell_i32_random_d2(void)
{
	alloctests_sort_tests_stability_cell_i32_random_d2_impl();
}

/* alloctests_sort_tests_stability_cell_i32_random_d20_impl_closure_1 is alloctests_sort_tests_stability_cell_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_cell_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_stability_cell_i32_random_d20_impl_closure_0 is alloctests_sort_tests_stability_cell_i32_random_d20_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_cell_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_cell_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_cell_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_cell_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_cell_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_cell_i32_random_d20 is the test stability_cell_i32_random_d20. */
void
test_alloctests_sort_tests_stable_stability_cell_i32_random_d20(void)
{
	alloctests_sort_tests_stability_cell_i32_random_d20_impl();
}

/* alloctests_sort_tests_stability_cell_i32_random_s95_impl_closure_1 is alloctests_sort_tests_stability_cell_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_cell_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_stability_cell_i32_random_s95_impl_closure_0 is alloctests_sort_tests_stability_cell_i32_random_s95_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_cell_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_cell_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_cell_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_cell_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_cell_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_cell_i32_random_s95 is the test stability_cell_i32_random_s95. */
void
test_alloctests_sort_tests_stable_stability_cell_i32_random_s95(void)
{
	alloctests_sort_tests_stability_cell_i32_random_s95_impl();
}

/* alloctests_sort_tests_stability_cell_i32_ascending_impl_closure_0 is alloctests_sort_tests_stability_cell_i32_ascending_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_cell_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_cell_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_cell_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_cell_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_stability_cell_i32_ascending is the test stability_cell_i32_ascending. */
void
test_alloctests_sort_tests_stable_stability_cell_i32_ascending(void)
{
	alloctests_sort_tests_stability_cell_i32_ascending_impl();
}

/* alloctests_sort_tests_stability_cell_i32_descending_impl_closure_0 is alloctests_sort_tests_stability_cell_i32_descending_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_cell_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_cell_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_cell_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_cell_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_stability_cell_i32_descending is the test stability_cell_i32_descending. */
void
test_alloctests_sort_tests_stable_stability_cell_i32_descending(void)
{
	alloctests_sort_tests_stability_cell_i32_descending_impl();
}

/* alloctests_sort_tests_stability_cell_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_stability_cell_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_cell_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_stability_cell_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_stability_cell_i32_saw_mixed_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_cell_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_cell_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_cell_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_cell_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_cell_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_cell_i32_saw_mixed is the test stability_cell_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_stability_cell_i32_saw_mixed(void)
{
	alloctests_sort_tests_stability_cell_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_stability_string_random_impl_closure_0 is alloctests_sort_tests_stability_string_random_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_string_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_String(len, alloctests_sort_tests_string_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_string_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_string_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_string_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_stability_string_random is the test stability_string_random. */
void
test_alloctests_sort_tests_stable_stability_string_random(void)
{
	alloctests_sort_tests_stability_string_random_impl();
}

/* alloctests_sort_tests_stability_string_random_z1_impl_closure_1 is alloctests_sort_tests_stability_string_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_string_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_stability_string_random_z1_impl_closure_0 is alloctests_sort_tests_stability_string_random_z1_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_string_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_String(len, alloctests_sort_tests_string_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_string_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_string_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_string_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_string_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_string_random_z1 is the test stability_string_random_z1. */
void
test_alloctests_sort_tests_stable_stability_string_random_z1(void)
{
	alloctests_sort_tests_stability_string_random_z1_impl();
}

/* alloctests_sort_tests_stability_string_random_d2_impl_closure_1 is alloctests_sort_tests_stability_string_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_string_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_stability_string_random_d2_impl_closure_0 is alloctests_sort_tests_stability_string_random_d2_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_string_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_String(len, alloctests_sort_tests_string_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_string_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_string_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_string_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_string_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_string_random_d2 is the test stability_string_random_d2. */
void
test_alloctests_sort_tests_stable_stability_string_random_d2(void)
{
	alloctests_sort_tests_stability_string_random_d2_impl();
}

/* alloctests_sort_tests_stability_string_random_d20_impl_closure_1 is alloctests_sort_tests_stability_string_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_string_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_stability_string_random_d20_impl_closure_0 is alloctests_sort_tests_stability_string_random_d20_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_string_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_String(len, alloctests_sort_tests_string_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_string_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_string_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_string_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_string_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_string_random_d20 is the test stability_string_random_d20. */
void
test_alloctests_sort_tests_stable_stability_string_random_d20(void)
{
	alloctests_sort_tests_stability_string_random_d20_impl();
}

/* alloctests_sort_tests_stability_string_random_s95_impl_closure_1 is alloctests_sort_tests_stability_string_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_string_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_stability_string_random_s95_impl_closure_0 is alloctests_sort_tests_stability_string_random_s95_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_string_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_String(len, alloctests_sort_tests_string_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_string_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_string_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_string_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_string_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_string_random_s95 is the test stability_string_random_s95. */
void
test_alloctests_sort_tests_stable_stability_string_random_s95(void)
{
	alloctests_sort_tests_stability_string_random_s95_impl();
}

/* alloctests_sort_tests_stability_string_ascending_impl_closure_0 is alloctests_sort_tests_stability_string_ascending_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_string_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_String(len, alloctests_sort_tests_string_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_string_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_string_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_string_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_stability_string_ascending is the test stability_string_ascending. */
void
test_alloctests_sort_tests_stable_stability_string_ascending(void)
{
	alloctests_sort_tests_stability_string_ascending_impl();
}

/* alloctests_sort_tests_stability_string_descending_impl_closure_0 is alloctests_sort_tests_stability_string_descending_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_string_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_String(len, alloctests_sort_tests_string_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_string_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_string_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_string_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_stability_string_descending is the test stability_string_descending. */
void
test_alloctests_sort_tests_stable_stability_string_descending(void)
{
	alloctests_sort_tests_stability_string_descending_impl();
}

/* alloctests_sort_tests_stability_string_saw_mixed_impl_closure_1 is alloctests_sort_tests_stability_string_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_stability_string_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_stability_string_saw_mixed_impl_closure_0 is alloctests_sort_tests_stability_string_saw_mixed_impl's test, stability_with_patterns at the type. */
static void
alloctests_sort_tests_stability_string_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_stability_with_patterns_String(len, alloctests_sort_tests_string_from_i32_into, NULL, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_stability_string_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_stability_string_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 564, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_stability_string_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_stability_string_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_stability_string_saw_mixed is the test stability_string_saw_mixed. */
void
test_alloctests_sort_tests_stable_stability_string_saw_mixed(void)
{
	alloctests_sort_tests_stability_string_saw_mixed_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_i32_random_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random is the test panic_retain_orig_set_i32_random. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random(void)
{
	alloctests_sort_tests_panic_retain_orig_set_i32_random_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_z1 is the test panic_retain_orig_set_i32_random_z1. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_z1(void)
{
	alloctests_sort_tests_panic_retain_orig_set_i32_random_z1_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_d2 is the test panic_retain_orig_set_i32_random_d2. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_d2(void)
{
	alloctests_sort_tests_panic_retain_orig_set_i32_random_d2_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_d20 is the test panic_retain_orig_set_i32_random_d20. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_d20(void)
{
	alloctests_sort_tests_panic_retain_orig_set_i32_random_d20_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_s95 is the test panic_retain_orig_set_i32_random_s95. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_random_s95(void)
{
	alloctests_sort_tests_panic_retain_orig_set_i32_random_s95_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_ascending_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_i32_ascending_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_ascending is the test panic_retain_orig_set_i32_ascending. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_ascending(void)
{
	alloctests_sort_tests_panic_retain_orig_set_i32_ascending_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_descending_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_i32_descending_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_descending is the test panic_retain_orig_set_i32_descending. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_descending(void)
{
	alloctests_sort_tests_panic_retain_orig_set_i32_descending_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_saw_mixed is the test panic_retain_orig_set_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_i32_saw_mixed(void)
{
	alloctests_sort_tests_panic_retain_orig_set_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random is the test panic_retain_orig_set_cell_i32_random. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random(void)
{
	alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_z1 is the test panic_retain_orig_set_cell_i32_random_z1. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_z1(void)
{
	alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_z1_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_d2 is the test panic_retain_orig_set_cell_i32_random_d2. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_d2(void)
{
	alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d2_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_d20 is the test panic_retain_orig_set_cell_i32_random_d20. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_d20(void)
{
	alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_d20_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_s95 is the test panic_retain_orig_set_cell_i32_random_s95. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_random_s95(void)
{
	alloctests_sort_tests_panic_retain_orig_set_cell_i32_random_s95_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_ascending_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_ascending_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_cell_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_ascending is the test panic_retain_orig_set_cell_i32_ascending. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_ascending(void)
{
	alloctests_sort_tests_panic_retain_orig_set_cell_i32_ascending_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_descending_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_descending_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_cell_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_descending is the test panic_retain_orig_set_cell_i32_descending. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_descending(void)
{
	alloctests_sort_tests_panic_retain_orig_set_cell_i32_descending_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_saw_mixed is the test panic_retain_orig_set_cell_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_cell_i32_saw_mixed(void)
{
	alloctests_sort_tests_panic_retain_orig_set_cell_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_string_random_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_string_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random is the test panic_retain_orig_set_string_random. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random(void)
{
	alloctests_sort_tests_panic_retain_orig_set_string_random_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_z1 is the test panic_retain_orig_set_string_random_z1. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_z1(void)
{
	alloctests_sort_tests_panic_retain_orig_set_string_random_z1_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_d2 is the test panic_retain_orig_set_string_random_d2. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_d2(void)
{
	alloctests_sort_tests_panic_retain_orig_set_string_random_d2_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_d20 is the test panic_retain_orig_set_string_random_d20. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_d20(void)
{
	alloctests_sort_tests_panic_retain_orig_set_string_random_d20_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_s95 is the test panic_retain_orig_set_string_random_s95. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_string_random_s95(void)
{
	alloctests_sort_tests_panic_retain_orig_set_string_random_s95_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_string_ascending_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_string_ascending_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_string_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_string_ascending is the test panic_retain_orig_set_string_ascending. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_string_ascending(void)
{
	alloctests_sort_tests_panic_retain_orig_set_string_ascending_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_string_descending_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_string_descending_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_string_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_string_descending is the test panic_retain_orig_set_string_descending. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_string_descending(void)
{
	alloctests_sort_tests_panic_retain_orig_set_string_descending_impl();
}

/* alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl_closure_1 is alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl_closure_0 is alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl's test, panic_retain_orig_set at the type. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_panic_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 638, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_panic_retain_orig_set_string_saw_mixed is the test panic_retain_orig_set_string_saw_mixed. */
void
test_alloctests_sort_tests_stable_panic_retain_orig_set_string_saw_mixed(void)
{
	alloctests_sort_tests_panic_retain_orig_set_string_saw_mixed_impl();
}

/* alloctests_sort_tests_deterministic_i32_random_impl_closure_0 is alloctests_sort_tests_deterministic_i32_random_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_deterministic_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_deterministic_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_deterministic_i32_random is the test deterministic_i32_random. */
void
test_alloctests_sort_tests_stable_deterministic_i32_random(void)
{
	alloctests_sort_tests_deterministic_i32_random_impl();
}

/* alloctests_sort_tests_deterministic_i32_random_z1_impl_closure_1 is alloctests_sort_tests_deterministic_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_deterministic_i32_random_z1_impl_closure_0 is alloctests_sort_tests_deterministic_i32_random_z1_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_deterministic_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_deterministic_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_i32_random_z1 is the test deterministic_i32_random_z1. */
void
test_alloctests_sort_tests_stable_deterministic_i32_random_z1(void)
{
	alloctests_sort_tests_deterministic_i32_random_z1_impl();
}

/* alloctests_sort_tests_deterministic_i32_random_d2_impl_closure_1 is alloctests_sort_tests_deterministic_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_deterministic_i32_random_d2_impl_closure_0 is alloctests_sort_tests_deterministic_i32_random_d2_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_deterministic_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_deterministic_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_i32_random_d2 is the test deterministic_i32_random_d2. */
void
test_alloctests_sort_tests_stable_deterministic_i32_random_d2(void)
{
	alloctests_sort_tests_deterministic_i32_random_d2_impl();
}

/* alloctests_sort_tests_deterministic_i32_random_d20_impl_closure_1 is alloctests_sort_tests_deterministic_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_deterministic_i32_random_d20_impl_closure_0 is alloctests_sort_tests_deterministic_i32_random_d20_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_deterministic_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_deterministic_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_i32_random_d20 is the test deterministic_i32_random_d20. */
void
test_alloctests_sort_tests_stable_deterministic_i32_random_d20(void)
{
	alloctests_sort_tests_deterministic_i32_random_d20_impl();
}

/* alloctests_sort_tests_deterministic_i32_random_s95_impl_closure_1 is alloctests_sort_tests_deterministic_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_deterministic_i32_random_s95_impl_closure_0 is alloctests_sort_tests_deterministic_i32_random_s95_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_deterministic_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_deterministic_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_i32_random_s95 is the test deterministic_i32_random_s95. */
void
test_alloctests_sort_tests_stable_deterministic_i32_random_s95(void)
{
	alloctests_sort_tests_deterministic_i32_random_s95_impl();
}

/* alloctests_sort_tests_deterministic_i32_ascending_impl_closure_0 is alloctests_sort_tests_deterministic_i32_ascending_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_deterministic_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_deterministic_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_deterministic_i32_ascending is the test deterministic_i32_ascending. */
void
test_alloctests_sort_tests_stable_deterministic_i32_ascending(void)
{
	alloctests_sort_tests_deterministic_i32_ascending_impl();
}

/* alloctests_sort_tests_deterministic_i32_descending_impl_closure_0 is alloctests_sort_tests_deterministic_i32_descending_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_deterministic_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_deterministic_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_deterministic_i32_descending is the test deterministic_i32_descending. */
void
test_alloctests_sort_tests_stable_deterministic_i32_descending(void)
{
	alloctests_sort_tests_deterministic_i32_descending_impl();
}

/* alloctests_sort_tests_deterministic_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_deterministic_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_deterministic_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_deterministic_i32_saw_mixed_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_deterministic_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_deterministic_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_i32_saw_mixed is the test deterministic_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_deterministic_i32_saw_mixed(void)
{
	alloctests_sort_tests_deterministic_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_deterministic_cell_i32_random_impl_closure_0 is alloctests_sort_tests_deterministic_cell_i32_random_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_cell_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_deterministic_cell_i32_random is the test deterministic_cell_i32_random. */
void
test_alloctests_sort_tests_stable_deterministic_cell_i32_random(void)
{
	alloctests_sort_tests_deterministic_cell_i32_random_impl();
}

/* alloctests_sort_tests_deterministic_cell_i32_random_z1_impl_closure_1 is alloctests_sort_tests_deterministic_cell_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_z1_impl_closure_0 is alloctests_sort_tests_deterministic_cell_i32_random_z1_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_cell_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_cell_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_cell_i32_random_z1 is the test deterministic_cell_i32_random_z1. */
void
test_alloctests_sort_tests_stable_deterministic_cell_i32_random_z1(void)
{
	alloctests_sort_tests_deterministic_cell_i32_random_z1_impl();
}

/* alloctests_sort_tests_deterministic_cell_i32_random_d2_impl_closure_1 is alloctests_sort_tests_deterministic_cell_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_d2_impl_closure_0 is alloctests_sort_tests_deterministic_cell_i32_random_d2_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_cell_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_cell_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_cell_i32_random_d2 is the test deterministic_cell_i32_random_d2. */
void
test_alloctests_sort_tests_stable_deterministic_cell_i32_random_d2(void)
{
	alloctests_sort_tests_deterministic_cell_i32_random_d2_impl();
}

/* alloctests_sort_tests_deterministic_cell_i32_random_d20_impl_closure_1 is alloctests_sort_tests_deterministic_cell_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_d20_impl_closure_0 is alloctests_sort_tests_deterministic_cell_i32_random_d20_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_cell_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_cell_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_cell_i32_random_d20 is the test deterministic_cell_i32_random_d20. */
void
test_alloctests_sort_tests_stable_deterministic_cell_i32_random_d20(void)
{
	alloctests_sort_tests_deterministic_cell_i32_random_d20_impl();
}

/* alloctests_sort_tests_deterministic_cell_i32_random_s95_impl_closure_1 is alloctests_sort_tests_deterministic_cell_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_s95_impl_closure_0 is alloctests_sort_tests_deterministic_cell_i32_random_s95_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_cell_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_cell_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_cell_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_cell_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_cell_i32_random_s95 is the test deterministic_cell_i32_random_s95. */
void
test_alloctests_sort_tests_stable_deterministic_cell_i32_random_s95(void)
{
	alloctests_sort_tests_deterministic_cell_i32_random_s95_impl();
}

/* alloctests_sort_tests_deterministic_cell_i32_ascending_impl_closure_0 is alloctests_sort_tests_deterministic_cell_i32_ascending_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_cell_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_cell_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_cell_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_cell_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_deterministic_cell_i32_ascending is the test deterministic_cell_i32_ascending. */
void
test_alloctests_sort_tests_stable_deterministic_cell_i32_ascending(void)
{
	alloctests_sort_tests_deterministic_cell_i32_ascending_impl();
}

/* alloctests_sort_tests_deterministic_cell_i32_descending_impl_closure_0 is alloctests_sort_tests_deterministic_cell_i32_descending_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_cell_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_cell_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_cell_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_cell_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_deterministic_cell_i32_descending is the test deterministic_cell_i32_descending. */
void
test_alloctests_sort_tests_stable_deterministic_cell_i32_descending(void)
{
	alloctests_sort_tests_deterministic_cell_i32_descending_impl();
}

/* alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_cell_i32_saw_mixed is the test deterministic_cell_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_deterministic_cell_i32_saw_mixed(void)
{
	alloctests_sort_tests_deterministic_cell_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_deterministic_string_random_impl_closure_0 is alloctests_sort_tests_deterministic_string_random_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_string_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_string_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_string_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_string_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_deterministic_string_random is the test deterministic_string_random. */
void
test_alloctests_sort_tests_stable_deterministic_string_random(void)
{
	alloctests_sort_tests_deterministic_string_random_impl();
}

/* alloctests_sort_tests_deterministic_string_random_z1_impl_closure_1 is alloctests_sort_tests_deterministic_string_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_string_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_deterministic_string_random_z1_impl_closure_0 is alloctests_sort_tests_deterministic_string_random_z1_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_string_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_string_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_string_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_string_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_string_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_string_random_z1 is the test deterministic_string_random_z1. */
void
test_alloctests_sort_tests_stable_deterministic_string_random_z1(void)
{
	alloctests_sort_tests_deterministic_string_random_z1_impl();
}

/* alloctests_sort_tests_deterministic_string_random_d2_impl_closure_1 is alloctests_sort_tests_deterministic_string_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_string_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_deterministic_string_random_d2_impl_closure_0 is alloctests_sort_tests_deterministic_string_random_d2_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_string_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_string_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_string_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_string_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_string_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_string_random_d2 is the test deterministic_string_random_d2. */
void
test_alloctests_sort_tests_stable_deterministic_string_random_d2(void)
{
	alloctests_sort_tests_deterministic_string_random_d2_impl();
}

/* alloctests_sort_tests_deterministic_string_random_d20_impl_closure_1 is alloctests_sort_tests_deterministic_string_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_string_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_deterministic_string_random_d20_impl_closure_0 is alloctests_sort_tests_deterministic_string_random_d20_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_string_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_string_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_string_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_string_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_string_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_string_random_d20 is the test deterministic_string_random_d20. */
void
test_alloctests_sort_tests_stable_deterministic_string_random_d20(void)
{
	alloctests_sort_tests_deterministic_string_random_d20_impl();
}

/* alloctests_sort_tests_deterministic_string_random_s95_impl_closure_1 is alloctests_sort_tests_deterministic_string_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_string_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_deterministic_string_random_s95_impl_closure_0 is alloctests_sort_tests_deterministic_string_random_s95_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_string_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_string_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_string_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_string_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_string_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_string_random_s95 is the test deterministic_string_random_s95. */
void
test_alloctests_sort_tests_stable_deterministic_string_random_s95(void)
{
	alloctests_sort_tests_deterministic_string_random_s95_impl();
}

/* alloctests_sort_tests_deterministic_string_ascending_impl_closure_0 is alloctests_sort_tests_deterministic_string_ascending_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_string_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_string_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_string_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_string_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_deterministic_string_ascending is the test deterministic_string_ascending. */
void
test_alloctests_sort_tests_stable_deterministic_string_ascending(void)
{
	alloctests_sort_tests_deterministic_string_ascending_impl();
}

/* alloctests_sort_tests_deterministic_string_descending_impl_closure_0 is alloctests_sort_tests_deterministic_string_descending_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_string_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_string_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_string_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_string_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_deterministic_string_descending is the test deterministic_string_descending. */
void
test_alloctests_sort_tests_stable_deterministic_string_descending(void)
{
	alloctests_sort_tests_deterministic_string_descending_impl();
}

/* alloctests_sort_tests_deterministic_string_saw_mixed_impl_closure_1 is alloctests_sort_tests_deterministic_string_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_deterministic_string_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_deterministic_string_saw_mixed_impl_closure_0 is alloctests_sort_tests_deterministic_string_saw_mixed_impl's test, deterministic at the type. */
static void
alloctests_sort_tests_deterministic_string_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_deterministic_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_deterministic_string_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_deterministic_string_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 734, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_deterministic_string_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_deterministic_string_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_deterministic_string_saw_mixed is the test deterministic_string_saw_mixed. */
void
test_alloctests_sort_tests_stable_deterministic_string_saw_mixed(void)
{
	alloctests_sort_tests_deterministic_string_saw_mixed_impl();
}

/* alloctests_sort_tests_self_cmp_i32_random_impl_closure_0 is alloctests_sort_tests_self_cmp_i32_random_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_self_cmp_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_self_cmp_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_self_cmp_i32_random is the test self_cmp_i32_random. */
void
test_alloctests_sort_tests_stable_self_cmp_i32_random(void)
{
	alloctests_sort_tests_self_cmp_i32_random_impl();
}

/* alloctests_sort_tests_self_cmp_i32_random_z1_impl_closure_1 is alloctests_sort_tests_self_cmp_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_self_cmp_i32_random_z1_impl_closure_0 is alloctests_sort_tests_self_cmp_i32_random_z1_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_self_cmp_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_self_cmp_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_i32_random_z1 is the test self_cmp_i32_random_z1. */
void
test_alloctests_sort_tests_stable_self_cmp_i32_random_z1(void)
{
	alloctests_sort_tests_self_cmp_i32_random_z1_impl();
}

/* alloctests_sort_tests_self_cmp_i32_random_d2_impl_closure_1 is alloctests_sort_tests_self_cmp_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_self_cmp_i32_random_d2_impl_closure_0 is alloctests_sort_tests_self_cmp_i32_random_d2_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_self_cmp_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_self_cmp_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_i32_random_d2 is the test self_cmp_i32_random_d2. */
void
test_alloctests_sort_tests_stable_self_cmp_i32_random_d2(void)
{
	alloctests_sort_tests_self_cmp_i32_random_d2_impl();
}

/* alloctests_sort_tests_self_cmp_i32_random_d20_impl_closure_1 is alloctests_sort_tests_self_cmp_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_self_cmp_i32_random_d20_impl_closure_0 is alloctests_sort_tests_self_cmp_i32_random_d20_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_self_cmp_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_self_cmp_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_i32_random_d20 is the test self_cmp_i32_random_d20. */
void
test_alloctests_sort_tests_stable_self_cmp_i32_random_d20(void)
{
	alloctests_sort_tests_self_cmp_i32_random_d20_impl();
}

/* alloctests_sort_tests_self_cmp_i32_random_s95_impl_closure_1 is alloctests_sort_tests_self_cmp_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_self_cmp_i32_random_s95_impl_closure_0 is alloctests_sort_tests_self_cmp_i32_random_s95_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_self_cmp_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_self_cmp_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_i32_random_s95 is the test self_cmp_i32_random_s95. */
void
test_alloctests_sort_tests_stable_self_cmp_i32_random_s95(void)
{
	alloctests_sort_tests_self_cmp_i32_random_s95_impl();
}

/* alloctests_sort_tests_self_cmp_i32_ascending_impl_closure_0 is alloctests_sort_tests_self_cmp_i32_ascending_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_self_cmp_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_self_cmp_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_self_cmp_i32_ascending is the test self_cmp_i32_ascending. */
void
test_alloctests_sort_tests_stable_self_cmp_i32_ascending(void)
{
	alloctests_sort_tests_self_cmp_i32_ascending_impl();
}

/* alloctests_sort_tests_self_cmp_i32_descending_impl_closure_0 is alloctests_sort_tests_self_cmp_i32_descending_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_self_cmp_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_self_cmp_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_self_cmp_i32_descending is the test self_cmp_i32_descending. */
void
test_alloctests_sort_tests_stable_self_cmp_i32_descending(void)
{
	alloctests_sort_tests_self_cmp_i32_descending_impl();
}

/* alloctests_sort_tests_self_cmp_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_self_cmp_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_self_cmp_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_self_cmp_i32_saw_mixed_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_self_cmp_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_self_cmp_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_i32_saw_mixed is the test self_cmp_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_self_cmp_i32_saw_mixed(void)
{
	alloctests_sort_tests_self_cmp_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_impl_closure_0 is alloctests_sort_tests_self_cmp_cell_i32_random_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_cell_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_self_cmp_cell_i32_random is the test self_cmp_cell_i32_random. */
void
test_alloctests_sort_tests_stable_self_cmp_cell_i32_random(void)
{
	alloctests_sort_tests_self_cmp_cell_i32_random_impl();
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl_closure_1 is alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl_closure_0 is alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_z1 is the test self_cmp_cell_i32_random_z1. */
void
test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_z1(void)
{
	alloctests_sort_tests_self_cmp_cell_i32_random_z1_impl();
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl_closure_1 is alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl_closure_0 is alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_d2 is the test self_cmp_cell_i32_random_d2. */
void
test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_d2(void)
{
	alloctests_sort_tests_self_cmp_cell_i32_random_d2_impl();
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl_closure_1 is alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl_closure_0 is alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_d20 is the test self_cmp_cell_i32_random_d20. */
void
test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_d20(void)
{
	alloctests_sort_tests_self_cmp_cell_i32_random_d20_impl();
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl_closure_1 is alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl_closure_0 is alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_s95 is the test self_cmp_cell_i32_random_s95. */
void
test_alloctests_sort_tests_stable_self_cmp_cell_i32_random_s95(void)
{
	alloctests_sort_tests_self_cmp_cell_i32_random_s95_impl();
}

/* alloctests_sort_tests_self_cmp_cell_i32_ascending_impl_closure_0 is alloctests_sort_tests_self_cmp_cell_i32_ascending_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_cell_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_cell_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_cell_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_cell_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_self_cmp_cell_i32_ascending is the test self_cmp_cell_i32_ascending. */
void
test_alloctests_sort_tests_stable_self_cmp_cell_i32_ascending(void)
{
	alloctests_sort_tests_self_cmp_cell_i32_ascending_impl();
}

/* alloctests_sort_tests_self_cmp_cell_i32_descending_impl_closure_0 is alloctests_sort_tests_self_cmp_cell_i32_descending_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_cell_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_cell_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_cell_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_cell_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_self_cmp_cell_i32_descending is the test self_cmp_cell_i32_descending. */
void
test_alloctests_sort_tests_stable_self_cmp_cell_i32_descending(void)
{
	alloctests_sort_tests_self_cmp_cell_i32_descending_impl();
}

/* alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_cell_i32_saw_mixed is the test self_cmp_cell_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_self_cmp_cell_i32_saw_mixed(void)
{
	alloctests_sort_tests_self_cmp_cell_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_self_cmp_string_random_impl_closure_0 is alloctests_sort_tests_self_cmp_string_random_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_string_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_string_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_string_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_string_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_self_cmp_string_random is the test self_cmp_string_random. */
void
test_alloctests_sort_tests_stable_self_cmp_string_random(void)
{
	alloctests_sort_tests_self_cmp_string_random_impl();
}

/* alloctests_sort_tests_self_cmp_string_random_z1_impl_closure_1 is alloctests_sort_tests_self_cmp_string_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_string_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_self_cmp_string_random_z1_impl_closure_0 is alloctests_sort_tests_self_cmp_string_random_z1_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_string_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_string_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_string_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_string_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_string_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_string_random_z1 is the test self_cmp_string_random_z1. */
void
test_alloctests_sort_tests_stable_self_cmp_string_random_z1(void)
{
	alloctests_sort_tests_self_cmp_string_random_z1_impl();
}

/* alloctests_sort_tests_self_cmp_string_random_d2_impl_closure_1 is alloctests_sort_tests_self_cmp_string_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_string_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_self_cmp_string_random_d2_impl_closure_0 is alloctests_sort_tests_self_cmp_string_random_d2_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_string_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_string_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_string_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_string_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_string_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_string_random_d2 is the test self_cmp_string_random_d2. */
void
test_alloctests_sort_tests_stable_self_cmp_string_random_d2(void)
{
	alloctests_sort_tests_self_cmp_string_random_d2_impl();
}

/* alloctests_sort_tests_self_cmp_string_random_d20_impl_closure_1 is alloctests_sort_tests_self_cmp_string_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_string_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_self_cmp_string_random_d20_impl_closure_0 is alloctests_sort_tests_self_cmp_string_random_d20_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_string_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_string_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_string_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_string_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_string_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_string_random_d20 is the test self_cmp_string_random_d20. */
void
test_alloctests_sort_tests_stable_self_cmp_string_random_d20(void)
{
	alloctests_sort_tests_self_cmp_string_random_d20_impl();
}

/* alloctests_sort_tests_self_cmp_string_random_s95_impl_closure_1 is alloctests_sort_tests_self_cmp_string_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_string_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_self_cmp_string_random_s95_impl_closure_0 is alloctests_sort_tests_self_cmp_string_random_s95_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_string_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_string_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_string_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_string_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_string_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_string_random_s95 is the test self_cmp_string_random_s95. */
void
test_alloctests_sort_tests_stable_self_cmp_string_random_s95(void)
{
	alloctests_sort_tests_self_cmp_string_random_s95_impl();
}

/* alloctests_sort_tests_self_cmp_string_ascending_impl_closure_0 is alloctests_sort_tests_self_cmp_string_ascending_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_string_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_string_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_string_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_string_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_self_cmp_string_ascending is the test self_cmp_string_ascending. */
void
test_alloctests_sort_tests_stable_self_cmp_string_ascending(void)
{
	alloctests_sort_tests_self_cmp_string_ascending_impl();
}

/* alloctests_sort_tests_self_cmp_string_descending_impl_closure_0 is alloctests_sort_tests_self_cmp_string_descending_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_string_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_string_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_string_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_string_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_self_cmp_string_descending is the test self_cmp_string_descending. */
void
test_alloctests_sort_tests_stable_self_cmp_string_descending(void)
{
	alloctests_sort_tests_self_cmp_string_descending_impl();
}

/* alloctests_sort_tests_self_cmp_string_saw_mixed_impl_closure_1 is alloctests_sort_tests_self_cmp_string_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_self_cmp_string_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_self_cmp_string_saw_mixed_impl_closure_0 is alloctests_sort_tests_self_cmp_string_saw_mixed_impl's test, self_cmp at the type. */
static void
alloctests_sort_tests_self_cmp_string_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_self_cmp_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_self_cmp_string_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_self_cmp_string_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 761, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_self_cmp_string_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_self_cmp_string_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_self_cmp_string_saw_mixed is the test self_cmp_string_saw_mixed. */
void
test_alloctests_sort_tests_stable_self_cmp_string_saw_mixed(void)
{
	alloctests_sort_tests_self_cmp_string_saw_mixed_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random is the test violate_ord_retain_orig_set_i32_random. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_z1 is the test violate_ord_retain_orig_set_i32_random_z1. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_z1(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_z1_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_d2 is the test violate_ord_retain_orig_set_i32_random_d2. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_d2(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d2_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_d20 is the test violate_ord_retain_orig_set_i32_random_d20. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_d20(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_d20_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_s95 is the test violate_ord_retain_orig_set_i32_random_s95. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_random_s95(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_i32_random_s95_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_ascending_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_ascending_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_ascending is the test violate_ord_retain_orig_set_i32_ascending. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_ascending(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_i32_ascending_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_descending_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_descending_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_descending is the test violate_ord_retain_orig_set_i32_descending. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_descending(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_i32_descending_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_I32(len, alloctests_sort_tests_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_i32_ref_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_saw_mixed is the test violate_ord_retain_orig_set_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_i32_saw_mixed(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random is the test violate_ord_retain_orig_set_cell_i32_random. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_z1 is the test violate_ord_retain_orig_set_cell_i32_random_z1. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_z1(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_z1_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_d2 is the test violate_ord_retain_orig_set_cell_i32_random_d2. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_d2(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d2_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_d20 is the test violate_ord_retain_orig_set_cell_i32_random_d20. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_d20(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_d20_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_s95 is the test violate_ord_retain_orig_set_cell_i32_random_s95. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_random_s95(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_random_s95_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_ascending_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_ascending_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_ascending is the test violate_ord_retain_orig_set_cell_i32_ascending. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_ascending(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_ascending_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_descending_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_descending_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_descending is the test violate_ord_retain_orig_set_cell_i32_descending. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_descending(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_descending_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_CellI32(len, alloctests_sort_tests_cell_i32_from_i32_into, NULL, alloctests_sort_tests_i32_from_cell_i32_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_saw_mixed is the test violate_ord_retain_orig_set_cell_i32_saw_mixed. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_cell_i32_saw_mixed(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_cell_i32_saw_mixed_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_string_random_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random is the test violate_ord_retain_orig_set_string_random. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_string_random_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_z1 is the test violate_ord_retain_orig_set_string_random_z1. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_z1(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_string_random_z1_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_d2 is the test violate_ord_retain_orig_set_string_random_d2. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_d2(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d2_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_d20 is the test violate_ord_retain_orig_set_string_random_d20. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_d20(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_string_random_d20_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_s95 is the test violate_ord_retain_orig_set_string_random_s95. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_random_s95(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_string_random_s95_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_ascending_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_string_ascending_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_ascending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_string_ascending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_ascending is the test violate_ord_retain_orig_set_string_ascending. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_ascending(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_string_ascending_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_descending_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_string_descending_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_descending_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_string_descending_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_descending is the test violate_ord_retain_orig_set_string_descending. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_descending(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_string_descending_impl();
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl_closure_1 is alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl_closure_1(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl_closure_0 is alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl's test, violate_ord_retain_orig_set at the type. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl_closure_0(void *captures, size_t len, void (*pattern_fn) (size_t len, StdVec *result))
{
	alloctests_sort_tests_violate_ord_retain_orig_set_String(len, alloctests_sort_tests_string_from_i32_into, NULL, alloctests_sort_tests_i32_from_string_fn, NULL, NULL, pattern_fn);
}

/* alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 3]. */
static void
alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 923, 1);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 3; k < k_len; k++)
		alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl_closure_0(NULL, ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl_closure_1);
}

/* test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_saw_mixed is the test violate_ord_retain_orig_set_string_saw_mixed. */
void
test_alloctests_sort_tests_stable_violate_ord_retain_orig_set_string_saw_mixed(void)
{
	alloctests_sort_tests_violate_ord_retain_orig_set_string_saw_mixed_impl();
}

/* alloctests_sort_tests_observable_is_less_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_observable_is_less_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 595, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_observable_is_less_random is the test observable_is_less_random. */
void
test_alloctests_sort_tests_stable_observable_is_less_random(void)
{
	alloctests_sort_tests_observable_is_less_random_impl();
}

/* alloctests_sort_tests_observable_is_less_random_z1_impl_closure_0 is alloctests_sort_tests_observable_is_less_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_observable_is_less_random_z1_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_observable_is_less_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_observable_is_less_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 595, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_observable_is_less_random_z1_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_observable_is_less_random_z1 is the test observable_is_less_random_z1. */
void
test_alloctests_sort_tests_stable_observable_is_less_random_z1(void)
{
	alloctests_sort_tests_observable_is_less_random_z1_impl();
}

/* alloctests_sort_tests_observable_is_less_random_d2_impl_closure_0 is alloctests_sort_tests_observable_is_less_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_observable_is_less_random_d2_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_observable_is_less_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_observable_is_less_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 595, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_observable_is_less_random_d2_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_observable_is_less_random_d2 is the test observable_is_less_random_d2. */
void
test_alloctests_sort_tests_stable_observable_is_less_random_d2(void)
{
	alloctests_sort_tests_observable_is_less_random_d2_impl();
}

/* alloctests_sort_tests_observable_is_less_random_d20_impl_closure_0 is alloctests_sort_tests_observable_is_less_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_observable_is_less_random_d20_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_observable_is_less_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_observable_is_less_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 595, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_observable_is_less_random_d20_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_observable_is_less_random_d20 is the test observable_is_less_random_d20. */
void
test_alloctests_sort_tests_stable_observable_is_less_random_d20(void)
{
	alloctests_sort_tests_observable_is_less_random_d20_impl();
}

/* alloctests_sort_tests_observable_is_less_random_s95_impl_closure_0 is alloctests_sort_tests_observable_is_less_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_observable_is_less_random_s95_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_observable_is_less_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_observable_is_less_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 595, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_observable_is_less_random_s95_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_observable_is_less_random_s95 is the test observable_is_less_random_s95. */
void
test_alloctests_sort_tests_stable_observable_is_less_random_s95(void)
{
	alloctests_sort_tests_observable_is_less_random_s95_impl();
}

/* alloctests_sort_tests_observable_is_less_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_observable_is_less_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 595, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_observable_is_less_ascending is the test observable_is_less_ascending. */
void
test_alloctests_sort_tests_stable_observable_is_less_ascending(void)
{
	alloctests_sort_tests_observable_is_less_ascending_impl();
}

/* alloctests_sort_tests_observable_is_less_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_observable_is_less_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 595, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_observable_is_less_descending is the test observable_is_less_descending. */
void
test_alloctests_sort_tests_stable_observable_is_less_descending(void)
{
	alloctests_sort_tests_observable_is_less_descending_impl();
}

/* alloctests_sort_tests_observable_is_less_saw_mixed_impl_closure_0 is alloctests_sort_tests_observable_is_less_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_observable_is_less_saw_mixed_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_observable_is_less_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_observable_is_less_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 595, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_observable_is_less_saw_mixed_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_observable_is_less_saw_mixed is the test observable_is_less_saw_mixed. */
void
test_alloctests_sort_tests_stable_observable_is_less_saw_mixed(void)
{
	alloctests_sort_tests_observable_is_less_saw_mixed_impl();
}

/* alloctests_sort_tests_panic_observable_is_less_random_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_observable_is_less_random_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 695, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_random);
}

/* test_alloctests_sort_tests_stable_panic_observable_is_less_random is the test panic_observable_is_less_random. */
void
test_alloctests_sort_tests_stable_panic_observable_is_less_random(void)
{
	alloctests_sort_tests_panic_observable_is_less_random_impl();
}

/* alloctests_sort_tests_panic_observable_is_less_random_z1_impl_closure_0 is alloctests_sort_tests_panic_observable_is_less_random_z1_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_observable_is_less_random_z1_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_zipf(len, 1.0, result);
}

/* alloctests_sort_tests_panic_observable_is_less_random_z1_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_observable_is_less_random_z1_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 695, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_observable_is_less_random_z1_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_panic_observable_is_less_random_z1 is the test panic_observable_is_less_random_z1. */
void
test_alloctests_sort_tests_stable_panic_observable_is_less_random_z1(void)
{
	alloctests_sort_tests_panic_observable_is_less_random_z1_impl();
}

/* alloctests_sort_tests_panic_observable_is_less_random_d2_impl_closure_0 is alloctests_sort_tests_panic_observable_is_less_random_d2_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_observable_is_less_random_d2_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 2, result);
}

/* alloctests_sort_tests_panic_observable_is_less_random_d2_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_observable_is_less_random_d2_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 695, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_observable_is_less_random_d2_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_panic_observable_is_less_random_d2 is the test panic_observable_is_less_random_d2. */
void
test_alloctests_sort_tests_stable_panic_observable_is_less_random_d2(void)
{
	alloctests_sort_tests_panic_observable_is_less_random_d2_impl();
}

/* alloctests_sort_tests_panic_observable_is_less_random_d20_impl_closure_0 is alloctests_sort_tests_panic_observable_is_less_random_d20_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_observable_is_less_random_d20_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_uniform_RangeI32(len, 0, 16, result);
}

/* alloctests_sort_tests_panic_observable_is_less_random_d20_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_observable_is_less_random_d20_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 695, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_observable_is_less_random_d20_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_panic_observable_is_less_random_d20 is the test panic_observable_is_less_random_d20. */
void
test_alloctests_sort_tests_stable_panic_observable_is_less_random_d20(void)
{
	alloctests_sort_tests_panic_observable_is_less_random_d20_impl();
}

/* alloctests_sort_tests_panic_observable_is_less_random_s95_impl_closure_0 is alloctests_sort_tests_panic_observable_is_less_random_s95_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_observable_is_less_random_s95_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_random_sorted(len, 95.0, result);
}

/* alloctests_sort_tests_panic_observable_is_less_random_s95_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_observable_is_less_random_s95_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 695, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_observable_is_less_random_s95_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_panic_observable_is_less_random_s95 is the test panic_observable_is_less_random_s95. */
void
test_alloctests_sort_tests_stable_panic_observable_is_less_random_s95(void)
{
	alloctests_sort_tests_panic_observable_is_less_random_s95_impl();
}

/* alloctests_sort_tests_panic_observable_is_less_ascending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_observable_is_less_ascending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 695, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_ascending);
}

/* test_alloctests_sort_tests_stable_panic_observable_is_less_ascending is the test panic_observable_is_less_ascending. */
void
test_alloctests_sort_tests_stable_panic_observable_is_less_ascending(void)
{
	alloctests_sort_tests_panic_observable_is_less_ascending_impl();
}

/* alloctests_sort_tests_panic_observable_is_less_descending_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_observable_is_less_descending_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 695, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_patterns_descending);
}

/* test_alloctests_sort_tests_stable_panic_observable_is_less_descending is the test panic_observable_is_less_descending. */
void
test_alloctests_sort_tests_stable_panic_observable_is_less_descending(void)
{
	alloctests_sort_tests_panic_observable_is_less_descending_impl();
}

/* alloctests_sort_tests_panic_observable_is_less_saw_mixed_impl_closure_0 is alloctests_sort_tests_panic_observable_is_less_saw_mixed_impl's pattern, as the fn(usize) -> Vec<i32> it coerces to. */
static void
alloctests_sort_tests_panic_observable_is_less_saw_mixed_impl_closure_0(size_t len, StdVec *result)
{
	alloctests_sort_patterns_saw_mixed(len, std_as_f64_usize(std_f64_round(std_f64_log2((float8) len))), result);
}

/* alloctests_sort_tests_panic_observable_is_less_saw_mixed_impl runs its test at each of TEST_LENGTHS[..TEST_LENGTHS.len() - 2]. */
static void
alloctests_sort_tests_panic_observable_is_less_saw_mixed_impl(void)
{
	std_range_to_check(lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2, lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS), RUST_FILE, 695, 18);
	for (size_t k = 0, k_len = lengthof(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS) - 2; k < k_len; k++)
		alloctests_sort_tests_panic_observable_is_less(ALLOCTESTS_SORT_TESTS_TEST_LENGTHS[k], alloctests_sort_tests_panic_observable_is_less_saw_mixed_impl_closure_0);
}

/* test_alloctests_sort_tests_stable_panic_observable_is_less_saw_mixed is the test panic_observable_is_less_saw_mixed. */
void
test_alloctests_sort_tests_stable_panic_observable_is_less_saw_mixed(void)
{
	alloctests_sort_tests_panic_observable_is_less_saw_mixed_impl();
}
