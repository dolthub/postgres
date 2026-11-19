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
 * The C port of core's sort tests' ffi_types.rs (library/alloctests/tests/sort,
 * Rust 1.99.1): the element types of FFI layout the tests sort, a kibibyte
 * of i64s ordered by three of them, and a pair of f64s ordered by their
 * quotient.
 */
#include "postgres.h"

#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_libm.h"
#include "doltgres/std/test_sort.h"

#define RUST_FILE STD_RUST_LIBRARY STD_SEP "alloctests" STD_SEP "tests" STD_SEP "sort" STD_SEP "ffi_types.rs"

/* alloctests_sort_ffi_types_FFIOneKibiByte_new returns the kibibyte whose i64s count up from the value. */
AlloctestsSortFfiTypesFFIOneKibiByte
alloctests_sort_ffi_types_FFIOneKibiByte_new(int32 val)
{
	AlloctestsSortFfiTypesFFIOneKibiByte result = {0};
	int64		val_i64 = (int64) val;

	for (size_t k = 0, k_len = lengthof(result.values); k < k_len; k++)
	{
		result.values[k] = val_i64;
		val_i64 = STD_ADD(i64, val_i64, 1, 17, 44);
	}
	return result;
}

/* alloctests_sort_ffi_types_FFIOneKibiByte_as_i64 returns the sum of the kibibyte's 11th, 55th, and 77th i64s. */
static int64
alloctests_sort_ffi_types_FFIOneKibiByte_as_i64(const AlloctestsSortFfiTypesFFIOneKibiByte *self)
{
	int64		sum = STD_ADD(i64, self->values[11], self->values[55], 23, 9);

	return STD_ADD(i64, sum, self->values[77], 23, 9);
}

/* alloctests_sort_ffi_types_FFIOneKibiByte_Ord_cmp is FFIOneKibiByte's Ord: by as_i64. */
StdOrdering
alloctests_sort_ffi_types_FFIOneKibiByte_Ord_cmp(const void *self_, const void *other_)
{
	int64		a = alloctests_sort_ffi_types_FFIOneKibiByte_as_i64(self_);
	int64		b = alloctests_sort_ffi_types_FFIOneKibiByte_as_i64(other_);

	return a < b ? STD_ORDERING_LESS : (a > b ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* alloctests_sort_ffi_types_FFIOneKibiByte_PartialOrd_lt is FFIOneKibiByte's PartialOrd::lt, the default's partial_cmp of Less, on pointers. */
bool
alloctests_sort_ffi_types_FFIOneKibiByte_PartialOrd_lt(const AlloctestsSortFfiTypesFFIOneKibiByte *self, const AlloctestsSortFfiTypesFFIOneKibiByte *other)
{
	return alloctests_sort_ffi_types_FFIOneKibiByte_Ord_cmp(self, other) == STD_ORDERING_LESS;
}

/* alloctests_sort_ffi_types_FFIOneKibiByte_PartialEq_eq is FFIOneKibiByte's derived PartialEq: every i64 equal. */
bool
alloctests_sort_ffi_types_FFIOneKibiByte_PartialEq_eq(const void *self_, const void *other_)
{
	const AlloctestsSortFfiTypesFFIOneKibiByte *self = self_;
	const AlloctestsSortFfiTypesFFIOneKibiByte *other = other_;

	for (size_t k = 0, k_len = lengthof(self->values); k < k_len; k++)
		if (self->values[k] != other->values[k])
			return false;
	return true;
}

/* alloctests_sort_ffi_types_Array128I64_Debug_fmt is [i64; 128]'s Debug: a list of the values. */
static bool
alloctests_sort_ffi_types_Array128I64_Debug_fmt(const void *value, StdFormatter *f)
{
	const int64 *values = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = 128; k < k_len; k++)
		std_DebugList_entry(&list, &values[k], std_i64_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* alloctests_sort_ffi_types_FFIOneKibiByte_Debug_fmt is FFIOneKibiByte's derived Debug. */
bool
alloctests_sort_ffi_types_FFIOneKibiByte_Debug_fmt(const void *value, StdFormatter *f)
{
	const AlloctestsSortFfiTypesFFIOneKibiByte *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "FFIOneKibiByte", 14);

	std_DebugStruct_field(&s, "values", 6, self->values, alloctests_sort_ffi_types_Array128I64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/*
 * alloctests_sort_ffi_types_F128_new returns the pair of the value moved
 * past i32's range plus a tenth, and that value's logarithm to base 4.1.
 */
AlloctestsSortFfiTypesF128
alloctests_sort_ffi_types_F128_new(int32 val)
{
	float8		val_f = ((float8) val) + ((float8) PG_INT32_MAX) + 10.0;
	AlloctestsSortFfiTypesF128 result;

	result.x = val_f + 0.1;
	result.y = std_f64_log(val_f, 4.1);
	STD_ASSERT(54, 9, result.y < result.x, "y < x");
	STD_ASSERT(55, 9, std_f64_is_normal(result.x) && std_f64_is_normal(result.y),
			   "x.is_normal() && y.is_normal()");
	return result;
}

/* alloctests_sort_ffi_types_F128_Ord_cmp is F128's Ord: by each pair's quotient, through f64's partial_cmp and unwrap_unchecked. */
StdOrdering
alloctests_sort_ffi_types_F128_Ord_cmp(const void *self_, const void *other_)
{
	const AlloctestsSortFfiTypesF128 *self = self_;
	const AlloctestsSortFfiTypesF128 *other = other_;
	float8		this_div = self->x / self->y;
	float8		other_div = other->x / other->y;

	if (std_f64_is_nan(this_div) || std_f64_is_nan(other_div))
		std_hint_unreachable_unchecked(RUST_FILE, 80, 51);
	return this_div < other_div ? STD_ORDERING_LESS :
		(this_div > other_div ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* alloctests_sort_ffi_types_F128_PartialOrd_lt is F128's PartialOrd::lt, the default's partial_cmp of Less, on pointers. */
bool
alloctests_sort_ffi_types_F128_PartialOrd_lt(const AlloctestsSortFfiTypesF128 *self, const AlloctestsSortFfiTypesF128 *other)
{
	return alloctests_sort_ffi_types_F128_Ord_cmp(self, other) == STD_ORDERING_LESS;
}

/* alloctests_sort_ffi_types_F128_PartialEq_eq is F128's derived PartialEq: both f64s equal. */
bool
alloctests_sort_ffi_types_F128_PartialEq_eq(const void *self_, const void *other_)
{
	const AlloctestsSortFfiTypesF128 *self = self_;
	const AlloctestsSortFfiTypesF128 *other = other_;

	return self->x == other->x && self->y == other->y;
}

/* alloctests_sort_ffi_types_F128_Debug_fmt is F128's derived Debug. */
bool
alloctests_sort_ffi_types_F128_Debug_fmt(const void *value, StdFormatter *f)
{
	const AlloctestsSortFfiTypesF128 *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "F128", 4);

	std_DebugStruct_field(&s, "x", 1, &self->x, std_f64_Debug_fmt);
	std_DebugStruct_field(&s, "y", 1, &self->y, std_f64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}
