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
 * The scan-key half of Postgres' nbtutils.c (access' nbtutils.rs): how a
 * btree scan's array keys are searched, set, and advanced as the scan reads
 * entries, and how an entry's values are checked against the scan's keys,
 * over the keys Postgres' own _bt_preprocess_keys preprocessed.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/nbtutils.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#ifdef DOLTGRES_TEST_ACCESS
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "access" STD_SEP "src" STD_SEP "nbtutils.rs"

/* The element layouts of nbtutils' vectors. */
#define ACCESS_NBTUTILS_BT_KEY sizeof(AccessNbtutilsBtKey), _Alignof(AccessNbtutilsBtKey)
#define ACCESS_NBTUTILS_ARRAY_KEY sizeof(AccessNbtutilsArrayKey), _Alignof(AccessNbtutilsArrayKey)
#define ACCESS_NBTUTILS_DATUM sizeof(uint64), _Alignof(uint64)

/*
 * ACCESS_NBTUTILS_SKIP_SENTINELS are the flags of a skip array's key whose
 * element is a sentinel rather than a value.
 */
#define ACCESS_NBTUTILS_SKIP_SENTINELS (ACCESS_NBTUTILS_SK_BT_MINVAL | ACCESS_NBTUTILS_SK_BT_MAXVAL | \
										ACCESS_NBTUTILS_SK_BT_NEXT | ACCESS_NBTUTILS_SK_BT_PRIOR)

/* ACCESS_NBTUTILS_KEY returns the key at a position of a scan's keys, checked as Rust's index is at a place. */
#define ACCESS_NBTUTILS_KEY(pre, at, line, col) \
	(std_index_check((at), (pre)->keys.len, RUST_FILE, (line), (col)), \
	 &STD_VEC_AT(&(pre)->keys, AccessNbtutilsBtKey, (at)))

/* ACCESS_NBTUTILS_ARRAY returns the array at a position of a scan's arrays, checked as Rust's index is at a place. */
#define ACCESS_NBTUTILS_ARRAY(pre, at, line, col) \
	(std_index_check((at), (pre)->arrays.len, RUST_FILE, (line), (col)), \
	 &STD_VEC_AT(&(pre)->arrays, AccessNbtutilsArrayKey, (at)))

/* ACCESS_NBTUTILS_ELEMENT returns an array's element at a position, checked as Rust's index is at a place. */
#define ACCESS_NBTUTILS_ELEMENT(array, at, line, col) \
	(std_index_check((at), (array)->elements.len, RUST_FILE, (line), (col)), \
	 STD_VEC_AT(&(array)->elements, uint64, (at)))

/* access_nbtutils_ScanDirection_is_forward reports whether the direction is forward. */
bool
access_nbtutils_ScanDirection_is_forward(AccessNbtutilsScanDirection self)
{
	return self == ACCESS_NBTUTILS_SCAN_DIRECTION_FORWARD;
}

/* access_nbtutils_ScanDirection_is_backward reports whether the direction is backward. */
bool
access_nbtutils_ScanDirection_is_backward(AccessNbtutilsScanDirection self)
{
	return self == ACCESS_NBTUTILS_SCAN_DIRECTION_BACKWARD;
}

/* access_nbtutils_ScanDirection_flipped returns the opposite direction. */
AccessNbtutilsScanDirection
access_nbtutils_ScanDirection_flipped(AccessNbtutilsScanDirection self)
{
	switch (self)
	{
		case ACCESS_NBTUTILS_SCAN_DIRECTION_BACKWARD:
			return ACCESS_NBTUTILS_SCAN_DIRECTION_FORWARD;
		case ACCESS_NBTUTILS_SCAN_DIRECTION_NO_MOVEMENT:
			return ACCESS_NBTUTILS_SCAN_DIRECTION_NO_MOVEMENT;
		case ACCESS_NBTUTILS_SCAN_DIRECTION_FORWARD:
			return ACCESS_NBTUTILS_SCAN_DIRECTION_BACKWARD;
	}
	pg_unreachable();
}

/* access_nbtutils_bt_key_clone is BtKey's Clone as a vector's element clone. */
static void
access_nbtutils_bt_key_clone(const void *key, void *result)
{
	access_nbtutils_BtKey_Clone_clone(key, result);
}

/* access_nbtutils_bt_key_drop is BtKey's drop glue as a vector's element. */
static void
access_nbtutils_bt_key_drop(void *key)
{
	access_nbtutils_BtKey_drop(key);
}

/* access_nbtutils_BtKey_Clone_clone copies a key, its row comparison's members with it. */
void
access_nbtutils_BtKey_Clone_clone(const AccessNbtutilsBtKey *self, AccessNbtutilsBtKey *result)
{
	*result = *self;
	std_slice_to_vec_clone(&result->members, self->members.ptr, self->members.len, ACCESS_NBTUTILS_BT_KEY,
						   access_nbtutils_bt_key_clone, access_nbtutils_bt_key_drop);
}

/* access_nbtutils_BtKey_drop frees a key's row comparison's members. */
void
access_nbtutils_BtKey_drop(AccessNbtutilsBtKey *self)
{
	std_vec_drop_Checked(&self->members, ACCESS_NBTUTILS_BT_KEY, access_nbtutils_bt_key_drop);
}

/* access_nbtutils_ArrayKey_drop frees an array's elements and the inequalities merged into it. */
void
access_nbtutils_ArrayKey_drop(AccessNbtutilsArrayKey *self)
{
	if (self->has_elements)
		std_vec_free_Checked(&self->elements, ACCESS_NBTUTILS_DATUM);
	if (self->has_low_compare)
		access_nbtutils_BtKey_drop(&self->low_compare);
	if (self->has_high_compare)
		access_nbtutils_BtKey_drop(&self->high_compare);
}

/* access_nbtutils_array_key_drop is ArrayKey's drop glue as a vector's element. */
static void
access_nbtutils_array_key_drop(void *array)
{
	access_nbtutils_ArrayKey_drop(array);
}

/* access_nbtutils_Preprocessed_drop frees a scan's keys and arrays. */
void
access_nbtutils_Preprocessed_drop(AccessNbtutilsPreprocessed *self)
{
	std_vec_drop_Checked(&self->keys, ACCESS_NBTUTILS_BT_KEY, access_nbtutils_bt_key_drop);
	std_vec_drop_Checked(&self->arrays, ACCESS_NBTUTILS_ARRAY_KEY, access_nbtutils_array_key_drop);
}

/*
 * access_nbtutils_invert_compare_result returns a comparison's result for the
 * opposite order, Postgres' INVERT_COMPARE_RESULT.
 */
int32
access_nbtutils_invert_compare_result(int32 result)
{
	return result < 0 ? 1 : -result;
}

/*
 * access_nbtutils_call_order answers how two values order by a three-way
 * ordering function, whose int32 is the word's low half.
 */
bool
access_nbtutils_call_order(const PgCatalogCaps *caps, AccessNbtutilsCalls calls, AccessNbtutilsFunction function,
						   uint32 collation, uint64 a, uint64 b, int32 *result, PgPgError *error)
{
	uint64		word;

	if (!calls.ops->call(calls.self, caps, function, collation, a, b, &word, error))
		return false;
	*result = (int32) (uint32) word;
	return true;
}

/* access_nbtutils_call_bool answers whether an operator's function holds for two values. */
static bool
access_nbtutils_call_bool(const PgCatalogCaps *caps, AccessNbtutilsCalls calls, AccessNbtutilsFunction function,
						  uint32 collation, uint64 a, uint64 b, bool *result, PgPgError *error)
{
	uint64		word;

	if (!calls.ops->call(calls.self, caps, function, collation, a, b, &word, error))
		return false;
	*result = (word & 0xff) != 0;
	return true;
}

/* access_nbtutils_internal returns an internal error of the fork's. */
static PgPgError
access_nbtutils_internal(const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX000", &text);
}

/* access_nbtutils_fail sets an internal error of the fork's and answers false. */
static bool
access_nbtutils_fail(const char *message, PgPgError *error)
{
	*error = access_nbtutils_internal(message);
	return false;
}

/*
 * access_nbtutils_compare_array_skey answers how an entry's value orders
 * against an array element in the index's order, NULLs placed as the
 * column places them, Postgres' _bt_compare_array_skey.
 */
bool
access_nbtutils_compare_array_skey(const PgCatalogCaps *caps, AccessNbtutilsCalls calls, bool has_order,
								   AccessNbtutilsFunction order, access_OptionU64_t tupdatum,
								   access_OptionU64_t arrdatum, const AccessNbtutilsBtKey *cur, int32 *result,
								   PgPgError *error)
{
	bool		nulls_first = (cur->flags & ACCESS_NBTUTILS_SK_BT_NULLS_FIRST) != 0;
	bool		has_arrdatum = arrdatum.has && (cur->flags & ACCESS_NBTUTILS_SK_ISNULL) == 0;
	int32		ordered;

	if (!tupdatum.has)
	{
		*result = !has_arrdatum ? 0 : nulls_first ? -1 : 1;
		return true;
	}
	if (!has_arrdatum)
	{
		*result = nulls_first ? 1 : -1;
		return true;
	}
	if (!has_order)
		return access_nbtutils_fail("an equality key without its ordering function", error);
	if (!access_nbtutils_call_order(caps, calls, order, cur->collation, tupdatum.value, arrdatum.value, &ordered,
									error))
		return false;
	*result = (cur->flags & ACCESS_NBTUTILS_SK_BT_DESC) != 0 ? access_nbtutils_invert_compare_result(ordered) : ordered;
	return true;
}

/* access_nbtutils_some returns an Option<u64> of a value. */
static access_OptionU64_t
access_nbtutils_some(uint64 value)
{
	access_OptionU64_t some = {true, value};

	return some;
}

/*
 * access_nbtutils_binsrch_array_skey answers the position of the first of
 * an array's elements at or after an entry's value in the scan's direction,
 * with how the value orders against it, searching only past the current
 * element when cur_elem_trig says that element was just passed, Postgres'
 * _bt_binsrch_array_skey.
 */
bool
access_nbtutils_binsrch_array_skey(const PgCatalogCaps *caps, AccessNbtutilsCalls calls, AccessNbtutilsFunction order,
								   bool cur_elem_trig, AccessNbtutilsScanDirection dir, access_OptionU64_t tupdatum,
								   const AccessNbtutilsArrayKey *array, const AccessNbtutilsBtKey *cur,
								   size_t *result_0, int32 *result_1, PgPgError *error)
{
	ssize_t		low_elem = 0;
	ssize_t		high_elem;
	ssize_t		mid_elem = -1;
	int32		result = 0;
	uint64		element;

	if (!array->has_elements)
		return access_nbtutils_fail("a skip array searched as a SAOP array", error);
	high_elem = STD_SUB(isize, (ssize_t) array->elements.len, 1, 250, 50);
	if (cur_elem_trig)
	{
		if (dir == ACCESS_NBTUTILS_SCAN_DIRECTION_FORWARD)
		{
			low_elem = STD_ADD(isize, (ssize_t) array->cur_elem, 1, 254, 24);
			if (high_elem >= low_elem)
			{
				element = ACCESS_NBTUTILS_ELEMENT(array, (size_t) low_elem, 257, 81);
				if (!access_nbtutils_compare_array_skey(caps, calls, true, order, tupdatum,
														access_nbtutils_some(element),
														cur, &result, error))
					return false;
				if (result <= 0)
				{
					*result_0 = (size_t) low_elem;
					*result_1 = result;
					return true;
				}
				mid_elem = low_elem;
				low_elem = STD_ADD(isize, low_elem, 1, 262, 17);
			}
			if (high_elem < low_elem)
			{
				*result_0 = (size_t) high_elem;
				*result_1 = 1;
				return true;
			}
		}
		else
		{
			high_elem = STD_SUB(isize, (ssize_t) array->cur_elem, 1, 268, 25);
			if (high_elem >= low_elem)
			{
				element = ACCESS_NBTUTILS_ELEMENT(array, (size_t) high_elem, 271, 81);
				if (!access_nbtutils_compare_array_skey(caps, calls, true, order, tupdatum,
														access_nbtutils_some(element),
														cur, &result, error))
					return false;
				if (result >= 0)
				{
					*result_0 = (size_t) high_elem;
					*result_1 = result;
					return true;
				}
				mid_elem = high_elem;
				high_elem = STD_SUB(isize, high_elem, 1, 276, 17);
			}
			if (high_elem < low_elem)
			{
				*result_0 = (size_t) low_elem;
				*result_1 = -1;
				return true;
			}
		}
	}
	result = 0;
	while (high_elem > low_elem)
	{
		mid_elem = STD_ADD(isize, low_elem, STD_SUB(isize, high_elem, low_elem, 285, 32) / 2, 285, 20);
		element = ACCESS_NBTUTILS_ELEMENT(array, (size_t) mid_elem, 286, 78);
		if (!access_nbtutils_compare_array_skey(caps, calls, true, order, tupdatum,
												access_nbtutils_some(element),
												cur, &result, error))
			return false;
		if (result == 0)
		{
			low_elem = mid_elem;
			break;
		}
		if (result > 0)
			low_elem = STD_ADD(isize, mid_elem, 1, 292, 24);
		else
			high_elem = mid_elem;
	}
	if (low_elem != mid_elem)
	{
		element = ACCESS_NBTUTILS_ELEMENT(array, (size_t) low_elem, 298, 78);
		if (!access_nbtutils_compare_array_skey(caps, calls, true, order, tupdatum, access_nbtutils_some(element), cur,
												&result, error))
			return false;
	}
	*result_0 = (size_t) low_elem;
	*result_1 = result;
	return true;
}

/* access_nbtutils_attno_index returns a key's column's index from its attribute number, as attno as usize - 1. */
static size_t
access_nbtutils_attno_index(int16 attno, uint32 line, uint32 col)
{
	return std_sub_usize((size_t) (ssize_t) attno, 1, RUST_FILE, line, col);
}

/* access_nbtutils_Checker_tupdatum returns an entry's value of a key's column. */
static access_OptionU64_t
access_nbtutils_Checker_tupdatum(const AccessNbtutilsChecker *self, const access_OptionU64_t *values,
								 size_t values_len, size_t key)
{
	size_t		at = (size_t) (ssize_t) ACCESS_NBTUTILS_KEY(self->pre, key, 320, 21)->attno - 1;
	access_OptionU64_t none = {0};

	return at < values_len ? values[at] : none;
}

/* access_nbtutils_Checker_array_of answers the position of the array of a key. */
static bool
access_nbtutils_Checker_array_of(const AccessNbtutilsChecker *self, size_t key, size_t *result, PgPgError *error)
{
	for (size_t i = 0; i < self->pre->arrays.len; i++)
	{
		if (STD_VEC_AT(&self->pre->arrays, AccessNbtutilsArrayKey, i).scan_key == key)
		{
			*result = i;
			return true;
		}
	}
	return access_nbtutils_fail("an array key without its array", error);
}

/* access_nbtutils_Checker_holds answers whether an operator's function holds for a value and a key's argument. */
static bool
access_nbtutils_Checker_holds(const AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
							  const AccessNbtutilsBtKey *key, uint64 value, uint64 argument, bool *result,
							  PgPgError *error)
{
	return access_nbtutils_call_bool(caps, self->calls, key->func, key->collation, value, argument, result, error);
}

/* access_nbtutils_Checker_fails answers whether an entry's value fails a skip array's bound, none failing no bound. */
static bool
access_nbtutils_Checker_fails(const AccessNbtutilsChecker *self, const PgCatalogCaps *caps, bool has_bound,
							  const AccessNbtutilsBtKey *bound, uint64 tupdatum, bool *result, PgPgError *error)
{
	bool		holds;

	if (!has_bound)
	{
		*result = false;
		return true;
	}
	if (!bound->has_argument)
		return access_nbtutils_fail("a bound without its value", error);
	if (!access_nbtutils_Checker_holds(self, caps, bound, tupdatum, bound->argument, &holds, error))
		return false;
	*result = !holds;
	return true;
}

/*
 * access_nbtutils_Checker_binsrch_skiparray_skey answers whether an entry's
 * value is below, within, or above the range of a skip array's values in
 * the index's order, -1, 0, or 1, checking only the bound ahead of the scan
 * when cur_elem_trig says the array was just passed, Postgres'
 * _bt_binsrch_skiparray_skey.
 */
static bool
access_nbtutils_Checker_binsrch_skiparray_skey(const AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
											   bool cur_elem_trig, access_OptionU64_t tupdatum, size_t array_at,
											   size_t key, int32 *result, PgPgError *error)
{
	const AccessNbtutilsArrayKey *array = ACCESS_NBTUTILS_ARRAY(self->pre, array_at, 360, 30);
	const AccessNbtutilsBtKey *cur = ACCESS_NBTUTILS_KEY(self->pre, key, 360, 55);
	bool		forward = access_nbtutils_ScanDirection_is_forward(self->dir);
	bool		has_near_bound = forward ? array->has_low_compare : array->has_high_compare;
	const AccessNbtutilsBtKey *near_bound = forward ? &array->low_compare : &array->high_compare;
	bool		has_far_bound = forward ? array->has_high_compare : array->has_low_compare;
	const AccessNbtutilsBtKey *far_bound = forward ? &array->high_compare : &array->low_compare;
	int32		near_result = forward ? -1 : 1;
	bool		failed;

	if (array->null_elem)
	{
		*result = 0;
		return true;
	}
	if (!tupdatum.has)
	{
		*result = (cur->flags & ACCESS_NBTUTILS_SK_BT_NULLS_FIRST) != 0 ? -1 : 1;
		return true;
	}
	if (!cur_elem_trig)
	{
		if (!access_nbtutils_Checker_fails(self, caps, has_near_bound, near_bound, tupdatum.value, &failed, error))
			return false;
		if (failed)
		{
			*result = near_result;
			return true;
		}
	}
	if (!access_nbtutils_Checker_fails(self, caps, has_far_bound, far_bound, tupdatum.value, &failed, error))
		return false;
	*result = failed ? -near_result : 0;
	return true;
}

/*
 * access_nbtutils_Checker_set_skip_argument sets a skip array's key's
 * argument, freeing the value it held, which the array copied.
 */
static void
access_nbtutils_Checker_set_skip_argument(AccessNbtutilsChecker *self, size_t key, bool has_argument, uint64 argument)
{
	size_t		column = access_nbtutils_attno_index(ACCESS_NBTUTILS_KEY(self->pre, key, 382, 22)->attno, 382, 22);
	AccessNbtutilsBtKey *skey = ACCESS_NBTUTILS_KEY(self->pre, key, 383, 51);
	bool		had = skey->has_argument;
	uint64		old = skey->argument;

	skey->has_argument = has_argument;
	skey->argument = argument;
	if (had)
		self->calls.ops->free(self->calls.self, column, old);
}

static void access_nbtutils_Checker_array_set_low_or_high(AccessNbtutilsChecker *self, size_t key, size_t array,
														  bool low_not_high);

/*
 * access_nbtutils_Checker_skiparray_set_isnull sets a skip array's element to
 * NULL, Postgres' _bt_skiparray_set_isnull.
 */
static void
access_nbtutils_Checker_skiparray_set_isnull(AccessNbtutilsChecker *self, size_t key)
{
	AccessNbtutilsBtKey *skey;

	access_nbtutils_Checker_set_skip_argument(self, key, false, 0);
	skey = ACCESS_NBTUTILS_KEY(self->pre, key, 415, 25);
	skey->flags &= ~ACCESS_NBTUTILS_SKIP_SENTINELS;
	skey->flags |= ACCESS_NBTUTILS_SK_SEARCHNULL | ACCESS_NBTUTILS_SK_ISNULL;
}

/*
 * access_nbtutils_Checker_skiparray_set_element sets a skip array's element
 * to a copy of an entry's value, NULL, or the end of its range the value is
 * past, Postgres' _bt_skiparray_set_element.
 */
static bool
access_nbtutils_Checker_skiparray_set_element(AccessNbtutilsChecker *self, const PgCatalogCaps *caps, size_t key,
											  size_t array, int32 set_elem_result, access_OptionU64_t tupdatum,
											  PgPgError *error)
{
	size_t		column;
	uint64		copied;

	if (set_elem_result != 0)
	{
		access_nbtutils_Checker_array_set_low_or_high(self, key, array, set_elem_result < 0);
		return true;
	}
	if (!tupdatum.has)
	{
		access_nbtutils_Checker_skiparray_set_isnull(self, key);
		return true;
	}
	column = access_nbtutils_attno_index(ACCESS_NBTUTILS_KEY(self->pre, key, 406, 44)->attno, 406, 44);
	if (!self->calls.ops->copy(self->calls.self, caps, column, tupdatum.value, &copied, error))
		return false;
	ACCESS_NBTUTILS_KEY(self->pre, key, 407, 9)->flags &= ~(ACCESS_NBTUTILS_SK_SEARCHNULL | ACCESS_NBTUTILS_SK_ISNULL |
															ACCESS_NBTUTILS_SKIP_SENTINELS);
	access_nbtutils_Checker_set_skip_argument(self, key, true, copied);
	return true;
}

/*
 * access_nbtutils_Checker_array_step moves an array one element on in a
 * direction, a skip array by its skip support or its NEXT or PRIOR
 * sentinel, answering false when it has no element left that way, Postgres'
 * _bt_array_increment and _bt_array_decrement.
 */
static bool
access_nbtutils_Checker_array_step(AccessNbtutilsChecker *self, const PgCatalogCaps *caps, size_t key,
								   size_t array, bool increment, bool *result, PgPgError *error)
{
	AccessNbtutilsArrayKey *state = ACCESS_NBTUTILS_ARRAY(self->pre, array, 424, 26);
	AccessNbtutilsBtKey *skey = ACCESS_NBTUTILS_KEY(self->pre, key, 425, 25);
	uint32		end;
	bool		null_at_end;
	bool		null_elem;
	size_t		column;
	uint64		copied;
	bool		has_stepped;
	uint64		stepped;
	const AccessNbtutilsArrayKey *bounded;
	bool		has_bound;
	const AccessNbtutilsBtKey *bound;
	bool		holds;

	if (state->has_elements)
	{
		if (increment && STD_ADD(usize, state->cur_elem, 1, 428, 25) < state->elements.len)
			state->cur_elem = STD_ADD(usize, state->cur_elem, 1, 428, 64);
		else if (!increment && state->cur_elem > 0)
			state->cur_elem = STD_SUB(usize, state->cur_elem, 1, 429, 48);
		else
		{
			*result = false;
			return true;
		}
		skey->has_argument = true;
		skey->argument = ACCESS_NBTUTILS_ELEMENT(state, state->cur_elem, 432, 34);
		*result = true;
		return true;
	}
	end = increment ? ACCESS_NBTUTILS_SK_BT_MAXVAL : ACCESS_NBTUTILS_SK_BT_MINVAL;
	null_at_end = ((skey->flags & ACCESS_NBTUTILS_SK_BT_NULLS_FIRST) != 0) != increment;
	if ((skey->flags & end) != 0 || ((skey->flags & ACCESS_NBTUTILS_SK_ISNULL) != 0 && null_at_end))
	{
		*result = false;
		return true;
	}
	if (!state->has_sksup)
	{
		skey->flags |= increment ? ACCESS_NBTUTILS_SK_BT_NEXT : ACCESS_NBTUTILS_SK_BT_PRIOR;
		*result = true;
		return true;
	}
	null_elem = state->null_elem;
	column = access_nbtutils_attno_index(skey->attno, 447, 22);
	if ((skey->flags & ACCESS_NBTUTILS_SK_ISNULL) != 0)
	{
		skey->flags &= ~(ACCESS_NBTUTILS_SK_SEARCHNULL | ACCESS_NBTUTILS_SK_ISNULL);
		if (!self->calls.ops->copy(self->calls.self, caps, column,
								   increment ? state->sksup.low_elem : state->sksup.high_elem, &copied, error))
			return false;
		access_nbtutils_Checker_set_skip_argument(self, key, true, copied);
		*result = true;
		return true;
	}
	if (!skey->has_argument)
		return access_nbtutils_fail("a skip array without its element", error);
	if (!self->calls.ops->step(self->calls.self, caps, array, skey->argument, increment, &has_stepped, &stepped,
							   error))
		return false;
	if (!has_stepped)
	{
		if (null_elem && null_at_end)
			access_nbtutils_Checker_skiparray_set_isnull(self, key);
		*result = null_elem && null_at_end;
		return true;
	}
	bounded = ACCESS_NBTUTILS_ARRAY(self->pre, array, 462, increment ? 37 : 83);
	has_bound = increment ? bounded->has_high_compare : bounded->has_low_compare;
	bound = increment ? &bounded->high_compare : &bounded->low_compare;
	if (has_bound)
	{
		if (!bound->has_argument)
			return access_nbtutils_fail("a bound without its value", error);
		if (!access_nbtutils_Checker_holds(self, caps, bound, stepped, bound->argument, &holds, error))
			return false;
		if (!holds)
		{
			self->calls.ops->free(self->calls.self, column, stepped);
			*result = false;
			return true;
		}
	}
	access_nbtutils_Checker_set_skip_argument(self, key, true, stepped);
	*result = true;
	return true;
}

/*
 * access_nbtutils_Checker_advance_array_keys_increment moves the arrays on
 * by one combination in the scan's direction, rolling the less significant
 * ones over to their first elements, and answers false when every array is
 * past its end, which sets them to their first elements for the other
 * direction, Postgres' _bt_advance_array_keys_increment.
 */
static bool
access_nbtutils_Checker_advance_array_keys_increment(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
													 bool *skip_array_set, bool *result, PgPgError *error)
{
	AccessNbtutilsScanDirection dir = self->dir;

	for (size_t array = self->pre->arrays.len; array-- > 0;)
	{
		size_t		key = ACCESS_NBTUTILS_ARRAY(self->pre, array, 480, 23)->scan_key;
		bool		stepped;

		if (!ACCESS_NBTUTILS_ARRAY(self->pre, array, 481, 16)->has_elements)
			*skip_array_set = true;
		if (!access_nbtutils_Checker_array_step(self, caps, key, array, access_nbtutils_ScanDirection_is_forward(dir),
												&stepped, error))
			return false;
		if (stepped)
		{
			*result = true;
			return true;
		}
		access_nbtutils_Checker_array_set_low_or_high(self, key, array, access_nbtutils_ScanDirection_is_forward(dir));
	}
	access_nbtutils_Checker_start_array_keys(self, access_nbtutils_ScanDirection_flipped(dir));
	*result = false;
	return true;
}

/*
 * access_nbtutils_Checker_order_proc answers the ordering function of a key,
 * which Postgres keeps only for a scan with arrays.
 */
static bool
access_nbtutils_Checker_order_proc(const AccessNbtutilsChecker *self, size_t key, AccessNbtutilsFunction *result)
{
	AccessNbtutilsFunction order = {.tag = ACCESS_NBTUTILS_FUNCTION_ORDER, .f0 = key};

	if (!self->pre->orders)
		return false;
	*result = order;
	return true;
}

/* access_nbtutils_datum returns a key's argument as a datum, none for a key whose argument is NULL. */
static access_OptionU64_t
access_nbtutils_datum(const AccessNbtutilsBtKey *key)
{
	access_OptionU64_t datum = {key->has_argument && (key->flags & ACCESS_NBTUTILS_SK_ISNULL) == 0, key->argument};

	return datum;
}

/*
 * access_nbtutils_Checker_tuple_before_array_skeys answers whether an entry
 * comes before the entries the current array and equality keys match in
 * the scan's direction, from a key on, stopping at the first key the
 * entry's value need not meet when readpagetup, Postgres'
 * _bt_tuple_before_array_skeys.
 */
static bool
access_nbtutils_Checker_tuple_before_array_skeys(const AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
												 const access_OptionU64_t *values, size_t values_len,
												 bool readpagetup, size_t sktrig, bool *result, PgPgError *error)
{
	for (size_t ikey = sktrig; ikey < self->pre->keys.len; ikey++)
	{
		const AccessNbtutilsBtKey *cur = ACCESS_NBTUTILS_KEY(self->pre, ikey, 504, 24);
		access_OptionU64_t tupdatum;
		int32		compared;

		if ((cur->flags & (ACCESS_NBTUTILS_SK_BT_REQFWD | ACCESS_NBTUTILS_SK_BT_REQBKWD)) == 0)
		{
			*result = false;
			return true;
		}
		if (cur->strategy != ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER)
		{
			if (readpagetup)
			{
				*result = false;
				return true;
			}
			continue;
		}
		tupdatum = access_nbtutils_Checker_tupdatum(self, values, values_len, ikey);
		if ((cur->flags & (ACCESS_NBTUTILS_SK_BT_MINVAL | ACCESS_NBTUTILS_SK_BT_MAXVAL)) == 0)
		{
			AccessNbtutilsFunction order;
			bool		has_order = access_nbtutils_Checker_order_proc(self, ikey, &order);

			if (!access_nbtutils_compare_array_skey(caps, self->calls, has_order, order, tupdatum,
													access_nbtutils_datum(cur), cur, &compared, error))
				return false;
			if (compared == 0)
			{
				if ((cur->flags & ACCESS_NBTUTILS_SK_BT_NEXT) != 0)
					compared = -1;
				else if ((cur->flags & ACCESS_NBTUTILS_SK_BT_PRIOR) != 0)
					compared = 1;
			}
		}
		else
		{
			size_t		array;

			if (!access_nbtutils_Checker_array_of(self, ikey, &array, error) ||
				!access_nbtutils_Checker_binsrch_skiparray_skey(self, caps, false, tupdatum, array, ikey, &compared,
																error))
				return false;
			if (compared == 0)
			{
				*result = false;
				return true;
			}
		}
		if ((access_nbtutils_ScanDirection_is_forward(self->dir) && compared < 0) ||
			(access_nbtutils_ScanDirection_is_backward(self->dir) && compared > 0))
		{
			*result = true;
			return true;
		}
		if (readpagetup || compared != 0)
		{
			*result = false;
			return true;
		}
	}
	*result = false;
	return true;
}

static bool access_nbtutils_Checker_check_compare(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
												  AccessNbtutilsScanDirection dir, const access_OptionU64_t *values,
												  size_t values_len, bool advancenonrequired, bool forcenonrequired,
												  bool *continuescan, size_t *ikey, bool *result, PgPgError *error);
static bool access_nbtutils_Checker_oppodir_checkkeys(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
													  const access_OptionU64_t *values, size_t values_len,
													  bool *result, PgPgError *error);

/*
 * access_nbtutils_Checker_advance_array_keys moves the arrays to the first
 * combination at or after an entry in the scan's direction and answers
 * whether the entry meets the advanced keys, ending the search of the index
 * where the entry does not and ending the scan once every array is past its
 * end, Postgres' _bt_advance_array_keys.
 */
bool
access_nbtutils_Checker_advance_array_keys(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
										   const access_OptionU64_t *values, size_t values_len, size_t sktrig,
										   bool sktrig_required, bool *continuescan, bool *result, PgPgError *error)
{
	AccessNbtutilsScanDirection dir = self->dir;
	size_t		arrayidx = 0;
	bool		beyond_end_advance = false;
	bool		skip_array_advanced = false;
	bool		has_required_opposite_direction_only = false;
	bool		all_required_satisfied = true;
	bool		all_satisfied = true;
	bool		checked;
	bool		new_prim_scan;

	if (!sktrig_required && STD_ADD(usize, sktrig, 1, 570, 32) < self->pre->keys.len)
	{
		size_t		last = STD_SUB(usize, self->pre->keys.len, 1, 571, 24);

		if ((ACCESS_NBTUTILS_KEY(self->pre, last, 572, 16)->flags & ACCESS_NBTUTILS_SK_SEARCHARRAY) == 0)
		{
			size_t		least_sign_ikey = last;
			bool		cont = true;

			if (!access_nbtutils_Checker_check_compare(self, caps, dir, values, values_len, false, false, &cont,
													   &least_sign_ikey, &checked, error))
				return false;
			if (!checked)
			{
				*result = false;
				return true;
			}
		}
	}
	for (size_t ikey = 0; ikey < self->pre->keys.len; ikey++)
	{
		bool		has_array = false;
		size_t		array = 0;
		const AccessNbtutilsBtKey *cur = ACCESS_NBTUTILS_KEY(self->pre, ikey, 581, 24);
		bool		required;
		access_OptionU64_t tupdatum;
		size_t		set_elem = 0;
		int32		compared;

		if (cur->strategy == ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER)
		{
			if ((cur->flags & ACCESS_NBTUTILS_SK_SEARCHARRAY) != 0)
			{
				has_array = true;
				array = arrayidx;
				arrayidx = STD_ADD(usize, arrayidx, 1, 585, 21);
			}
		}
		else if ((access_nbtutils_ScanDirection_is_forward(dir) && (cur->flags & ACCESS_NBTUTILS_SK_BT_REQBKWD) != 0) ||
				 (access_nbtutils_ScanDirection_is_backward(dir) && (cur->flags & ACCESS_NBTUTILS_SK_BT_REQFWD) != 0))
			has_required_opposite_direction_only = true;
		if (ikey < sktrig)
			continue;
		required = (cur->flags & (ACCESS_NBTUTILS_SK_BT_REQFWD | ACCESS_NBTUTILS_SK_BT_REQBKWD)) != 0;
		if (ikey == sktrig && !has_array)
		{
			beyond_end_advance = true;
			all_satisfied = false;
			all_required_satisfied = false;
			continue;
		}
		else if (cur->strategy != ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER || (!required && !has_array))
			continue;
		if (beyond_end_advance)
		{
			if (has_array)
				access_nbtutils_Checker_array_set_low_or_high(self, ikey, array,
															  access_nbtutils_ScanDirection_is_backward(dir));
			continue;
		}
		if (!all_required_satisfied)
		{
			if (has_array)
				access_nbtutils_Checker_array_set_low_or_high(self, ikey, array,
															  access_nbtutils_ScanDirection_is_forward(dir));
			continue;
		}
		tupdatum = access_nbtutils_Checker_tupdatum(self, values, values_len, ikey);
		if (has_array && !ACCESS_NBTUTILS_ARRAY(self->pre, array, 619, 32)->has_elements)
		{
			if (!access_nbtutils_Checker_binsrch_skiparray_skey(self, caps, sktrig_required && ikey == sktrig,
																tupdatum, array, ikey, &compared, error))
				return false;
		}
		else if (has_array)
		{
			AccessNbtutilsFunction order;
			bool		cur_elem_trig;
			const AccessNbtutilsArrayKey *searched;

			if (!access_nbtutils_Checker_order_proc(self, ikey, &order))
				return access_nbtutils_fail("an array key without its ordering function", error);
			cur_elem_trig = sktrig_required && ikey == sktrig;
			searched = ACCESS_NBTUTILS_ARRAY(self->pre, array, 632, 26);
			if (!access_nbtutils_binsrch_array_skey(caps, self->calls, order, cur_elem_trig, dir, tupdatum, searched,
													ACCESS_NBTUTILS_KEY(self->pre, ikey, 633, 26), &set_elem,
													&compared, error))
				return false;
		}
		else
		{
			const AccessNbtutilsBtKey *equal = ACCESS_NBTUTILS_KEY(self->pre, ikey, 639, 32);
			AccessNbtutilsFunction order;
			bool		has_order = access_nbtutils_Checker_order_proc(self, ikey, &order);

			if (!access_nbtutils_compare_array_skey(caps, self->calls, has_order, order, tupdatum,
													access_nbtutils_datum(equal), equal, &compared, error))
				return false;
		}
		if (sktrig_required && required &&
			((access_nbtutils_ScanDirection_is_forward(dir) && compared > 0) ||
			 (access_nbtutils_ScanDirection_is_backward(dir) && compared < 0)))
			beyond_end_advance = true;
		if (compared != 0)
		{
			all_satisfied = false;
			if (sktrig_required && required)
				all_required_satisfied = false;
			else
				break;
		}
		if (has_array)
		{
			if (!ACCESS_NBTUTILS_ARRAY(self->pre, array, 655, 20)->has_elements)
			{
				if (!access_nbtutils_Checker_skiparray_set_element(self, caps, ikey, array, compared, tupdatum, error))
					return false;
				skip_array_advanced = true;
			}
			else if (ACCESS_NBTUTILS_ARRAY(self->pre, array, 658, 27)->cur_elem != set_elem)
			{
				const AccessNbtutilsArrayKey *set = ACCESS_NBTUTILS_ARRAY(self->pre, array, 659, 35);
				bool		has_element = set->has_elements && set_elem < set->elements.len;
				uint64		element = has_element ? STD_VEC_AT(&set->elements, uint64, set_elem) : 0;
				AccessNbtutilsBtKey *skey;

				ACCESS_NBTUTILS_ARRAY(self->pre, array, 660, 21)->cur_elem = set_elem;
				skey = ACCESS_NBTUTILS_KEY(self->pre, ikey, 661, 21);
				skey->has_argument = has_element;
				skey->argument = element;
			}
		}
	}
	if (beyond_end_advance)
	{
		if (!access_nbtutils_Checker_advance_array_keys_increment(self, caps, &skip_array_advanced, &checked, error))
			return false;
		if (!checked)
		{
			*continuescan = false;
			*self->need_prim_scan = false;
			*result = false;
			return true;
		}
	}
	if ((sktrig_required && all_required_satisfied) || (!sktrig_required && all_satisfied))
	{
		size_t		nsktrig = STD_ADD(usize, sktrig, 1, 671, 44);
		bool		cont = true;

		if (!access_nbtutils_Checker_check_compare(self, caps, dir, values, values_len, false, !sktrig_required,
												   &cont, &nsktrig, &checked, error))
			return false;
		if (checked)
		{
			*continuescan = true;
			*result = true;
			return true;
		}
		if (!cont)
		{
			if (!access_nbtutils_Checker_advance_array_keys(self, caps, values, values_len, nsktrig, true,
															continuescan, &checked, error))
				return false;
			*result = false;
			return true;
		}
	}
	if (!sktrig_required)
	{
		*result = false;
		return true;
	}
	new_prim_scan = !all_required_satisfied;
	if (!new_prim_scan && has_required_opposite_direction_only)
	{
		if (!access_nbtutils_Checker_oppodir_checkkeys(self, caps, values, values_len, &checked, error))
			return false;
		new_prim_scan = !checked;
	}
	*continuescan = !new_prim_scan;
	*self->need_prim_scan = new_prim_scan;
	*result = false;
	return true;
}

/*
 * access_nbtutils_Checker_checkkeys answers whether an entry meets the
 * scan's keys, setting continuescan false where the search of the index
 * ends, advancing the arrays when the entry is past their current
 * elements, Postgres' _bt_checkkeys.
 */
bool
access_nbtutils_Checker_checkkeys(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
								  const access_OptionU64_t *values, size_t values_len, bool array_keys,
								  bool *continuescan, bool *result, PgPgError *error)
{
	size_t		ikey = 0;
	bool		res;
	bool		before;

	if (!access_nbtutils_Checker_check_compare(self, caps, self->dir, values, values_len, array_keys, false,
											   continuescan, &ikey, &res, error))
		return false;
	if (!array_keys || *continuescan)
	{
		*result = res;
		return true;
	}
	if (!access_nbtutils_Checker_tuple_before_array_skeys(self, caps, values, values_len, true, ikey, &before, error))
		return false;
	if (before)
	{
		*continuescan = true;
		*result = false;
		return true;
	}
	return access_nbtutils_Checker_advance_array_keys(self, caps, values, values_len, ikey, true, continuescan, result,
													  error);
}

/*
 * access_nbtutils_Checker_oppodir_checkkeys answers whether an entry meets
 * the inequalities required in the opposite direction only, Postgres'
 * _bt_oppodir_checkkeys.
 */
static bool
access_nbtutils_Checker_oppodir_checkkeys(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
										  const access_OptionU64_t *values, size_t values_len, bool *result,
										  PgPgError *error)
{
	size_t		ikey = 0;
	bool		cont = true;
	bool		checked;

	if (!access_nbtutils_Checker_check_compare(self, caps, access_nbtutils_ScanDirection_flipped(self->dir), values,
											   values_len, false, false, &cont, &ikey, &checked, error))
		return false;
	*result = cont ||
		ACCESS_NBTUTILS_KEY(self->pre, ikey, 718, 20)->strategy == ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER;
	return true;
}

/*
 * access_nbtutils_Checker_check_rowcompare_closure_0 is check_rowcompare's
 * required_in: whether a key is required in the direction.
 */
static bool
access_nbtutils_Checker_check_rowcompare_closure_0(AccessNbtutilsScanDirection dir, const AccessNbtutilsBtKey *key)
{
	return ((key->flags & ACCESS_NBTUTILS_SK_BT_REQFWD) != 0 && access_nbtutils_ScanDirection_is_forward(dir)) ||
		((key->flags & ACCESS_NBTUTILS_SK_BT_REQBKWD) != 0 && access_nbtutils_ScanDirection_is_backward(dir));
}

/*
 * access_nbtutils_Checker_check_rowcompare answers whether an entry's
 * values meet a row comparison, compared member by member until one
 * differs, ending the scan where the deciding member is required in the
 * scan's direction, Postgres' _bt_check_rowcompare.
 */
static bool
access_nbtutils_Checker_check_rowcompare(const AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
										 const AccessNbtutilsBtKey *header, const access_OptionU64_t *values,
										 size_t values_len, AccessNbtutilsScanDirection dir, bool forcenonrequired,
										 bool *continuescan, bool *result, PgPgError *error)
{
	int32		cmpresult = 0;
	size_t		deciding = 0;
	const AccessNbtutilsBtKey *subkey;
	char		message[64];

	for (size_t i = 0; i < header->members.len; i++)
	{
		size_t		at;

		subkey = &STD_VEC_AT(&header->members, AccessNbtutilsBtKey, i);
		deciding = i;
		if ((subkey->flags & ACCESS_NBTUTILS_SK_ISNULL) != 0)
		{
			if (!forcenonrequired && i > 0)
			{
				size_t		previous = STD_SUB(usize, i, 1, 832, 78);

				std_index_check(previous, header->members.len, RUST_FILE, 832, 63);
				if (access_nbtutils_Checker_check_rowcompare_closure_0(dir, &STD_VEC_AT(&header->members,
																						AccessNbtutilsBtKey,
																						previous)))
					*continuescan = false;
			}
			*result = false;
			return true;
		}
		at = access_nbtutils_attno_index(subkey->attno, 837, 42);
		if (at >= values_len || !values[at].has)
		{
			if (!forcenonrequired)
			{
				uint32		first = i == 0 ? ACCESS_NBTUTILS_SK_BT_REQFWD | ACCESS_NBTUTILS_SK_BT_REQBKWD : 0;
				bool		nulls_first = (subkey->flags & ACCESS_NBTUTILS_SK_BT_NULLS_FIRST) != 0;
				uint32		reqflags = nulls_first ?
					ACCESS_NBTUTILS_SK_BT_REQBKWD | (first & ACCESS_NBTUTILS_SK_BT_REQFWD) :
					ACCESS_NBTUTILS_SK_BT_REQFWD | (first & ACCESS_NBTUTILS_SK_BT_REQBKWD);
				bool		end = nulls_first ? access_nbtutils_ScanDirection_is_backward(dir) :
					access_nbtutils_ScanDirection_is_forward(dir);

				if ((subkey->flags & reqflags) != 0 && end)
					*continuescan = false;
			}
			*result = false;
			return true;
		}
		if (!subkey->has_argument)
			return access_nbtutils_fail("a row member without its value", error);
		if (!access_nbtutils_call_order(caps, self->calls, subkey->func, subkey->collation, values[at].value,
										subkey->argument, &cmpresult, error))
			return false;
		if ((subkey->flags & ACCESS_NBTUTILS_SK_BT_DESC) != 0)
			cmpresult = access_nbtutils_invert_compare_result(cmpresult);
		if (cmpresult != 0)
			break;
	}
	std_index_check(deciding, header->members.len, RUST_FILE, 859, 23);
	subkey = &STD_VEC_AT(&header->members, AccessNbtutilsBtKey, deciding);
	switch (subkey->strategy)
	{
		case ACCESS_NBTUTILS_BT_LESS_STRATEGY_NUMBER:
			*result = cmpresult < 0;
			break;
		case ACCESS_NBTUTILS_BT_LESS_EQUAL_STRATEGY_NUMBER:
			*result = cmpresult <= 0;
			break;
		case ACCESS_NBTUTILS_BT_GREATER_EQUAL_STRATEGY_NUMBER:
			*result = cmpresult >= 0;
			break;
		case ACCESS_NBTUTILS_BT_GREATER_STRATEGY_NUMBER:
			*result = cmpresult > 0;
			break;
		default:
			snprintf(message, sizeof(message), "unexpected strategy number %d", subkey->strategy);
			return access_nbtutils_fail(message, error);
	}
	if (!*result && !forcenonrequired && access_nbtutils_Checker_check_rowcompare_closure_0(dir, subkey))
		*continuescan = false;
	return true;
}

/*
 * access_nbtutils_Checker_check_compare checks an entry against the keys
 * from ikey on, leaving ikey at the first it fails, setting continuescan
 * false when a key required in the direction fails, and advancing a key's
 * array that is not required when advancenonrequired, Postgres'
 * _bt_check_compare.
 */
static bool
access_nbtutils_Checker_check_compare(AccessNbtutilsChecker *self, const PgCatalogCaps *caps,
									  AccessNbtutilsScanDirection dir, const access_OptionU64_t *values,
									  size_t values_len, bool advancenonrequired, bool forcenonrequired,
									  bool *continuescan, size_t *ikey, bool *result, PgPgError *error)
{
	*continuescan = true;
	while (*ikey < self->pre->keys.len)
	{
		const AccessNbtutilsBtKey *key = ACCESS_NBTUTILS_KEY(self->pre, *ikey, 737, 24);
		bool		required_same_dir = false;
		bool		required_opposite_dir_only = false;
		access_OptionU64_t value;
		bool		holds;
		bool		advanced = true;

		if (!forcenonrequired)
		{
			if (((key->flags & ACCESS_NBTUTILS_SK_BT_REQFWD) != 0 && access_nbtutils_ScanDirection_is_forward(dir)) ||
				((key->flags & ACCESS_NBTUTILS_SK_BT_REQBKWD) != 0 && access_nbtutils_ScanDirection_is_backward(dir)))
				required_same_dir = true;
			else if (((key->flags & ACCESS_NBTUTILS_SK_BT_REQFWD) != 0 &&
					  access_nbtutils_ScanDirection_is_backward(dir)) ||
					 ((key->flags & ACCESS_NBTUTILS_SK_BT_REQBKWD) != 0 &&
					  access_nbtutils_ScanDirection_is_forward(dir)))
				required_opposite_dir_only = true;
		}
		if ((key->flags & ACCESS_NBTUTILS_SKIP_SENTINELS) != 0)
		{
			if (forcenonrequired)
				return access_nbtutils_Checker_advance_array_keys(self, caps, values, values_len, *ikey, false,
																  &advanced, result, error);
			*continuescan = false;
			*result = false;
			return true;
		}
		if ((key->flags & ACCESS_NBTUTILS_SK_ROW_HEADER) != 0)
		{
			bool		met;

			if (!access_nbtutils_Checker_check_rowcompare(self, caps, key, values, values_len, dir, forcenonrequired,
														  continuescan, &met, error))
				return false;
			if (met)
			{
				*ikey = STD_ADD(usize, *ikey, 1, 759, 21);
				continue;
			}
			*result = false;
			return true;
		}
		value = access_nbtutils_Checker_tupdatum(self, values, values_len, *ikey);
		key = ACCESS_NBTUTILS_KEY(self->pre, *ikey, 765, 24);
		if ((key->flags & ACCESS_NBTUTILS_SK_ISNULL) != 0)
		{
			bool		satisfied = (key->flags & ACCESS_NBTUTILS_SK_SEARCHNULL) != 0 ? !value.has : value.has;

			if (satisfied)
			{
				*ikey = STD_ADD(usize, *ikey, 1, 772, 21);
				continue;
			}
			if (required_same_dir)
				*continuescan = false;
			else if ((key->flags & ACCESS_NBTUTILS_SK_BT_SKIP) != 0)
			{
				*ikey = STD_ADD(usize, *ikey, 1, 778, 21);
				continue;
			}
			*result = false;
			return true;
		}
		if (!value.has)
		{
			bool		end;

			if (forcenonrequired && (key->flags & ACCESS_NBTUTILS_SK_BT_SKIP) != 0)
				return access_nbtutils_Checker_advance_array_keys(self, caps, values, values_len, *ikey, false,
																  &advanced, result, error);
			end = (key->flags & ACCESS_NBTUTILS_SK_BT_NULLS_FIRST) != 0 ?
				access_nbtutils_ScanDirection_is_backward(dir) : access_nbtutils_ScanDirection_is_forward(dir);
			if ((required_same_dir || required_opposite_dir_only) && end)
				*continuescan = false;
			*result = false;
			return true;
		}
		if (!key->has_argument)
			return access_nbtutils_fail("a comparison key without its value", error);
		if (!access_nbtutils_Checker_holds(self, caps, key, value.value, key->argument, &holds, error))
			return false;
		if (!holds)
		{
			if (required_same_dir)
				*continuescan = false;
			else if (advancenonrequired && key->strategy == ACCESS_NBTUTILS_BT_EQUAL_STRATEGY_NUMBER &&
					 (key->flags & ACCESS_NBTUTILS_SK_SEARCHARRAY) != 0)
				return access_nbtutils_Checker_advance_array_keys(self, caps, values, values_len, *ikey, false,
																  &advanced, result, error);
			*result = false;
			return true;
		}
		*ikey = STD_ADD(usize, *ikey, 1, 808, 13);
	}
	*result = true;
	return true;
}

/*
 * access_nbtutils_Checker_start_array_keys sets every array to its first
 * element in a direction, Postgres' _bt_start_array_keys.
 */
void
access_nbtutils_Checker_start_array_keys(AccessNbtutilsChecker *self, AccessNbtutilsScanDirection dir)
{
	for (size_t array = 0; array < self->pre->arrays.len; array++)
	{
		size_t		key = ACCESS_NBTUTILS_ARRAY(self->pre, array, 876, 23)->scan_key;

		access_nbtutils_Checker_array_set_low_or_high(self, key, array, access_nbtutils_ScanDirection_is_forward(dir));
	}
}

/*
 * access_nbtutils_Checker_array_set_low_or_high sets an array to its lowest
 * or highest element, a skip array to NULL where NULLs sort at that end and
 * it has them, and otherwise to the sentinel of that end, Postgres'
 * _bt_array_set_low_or_high.
 */
static void
access_nbtutils_Checker_array_set_low_or_high(AccessNbtutilsChecker *self, size_t key, size_t array,
											  bool low_not_high)
{
	AccessNbtutilsArrayKey *state = ACCESS_NBTUTILS_ARRAY(self->pre, array, 884, 34);
	bool		null_elem;
	AccessNbtutilsBtKey *skey;

	if (state->has_elements)
	{
		size_t		set_elem = low_not_high ? 0 : STD_SUB(usize, state->elements.len, 1, 885, 57);
		uint64		element = ACCESS_NBTUTILS_ELEMENT(state, set_elem, 886, 27);

		ACCESS_NBTUTILS_ARRAY(self->pre, array, 887, 13)->cur_elem = set_elem;
		skey = ACCESS_NBTUTILS_KEY(self->pre, key, 888, 13);
		skey->has_argument = true;
		skey->argument = element;
		return;
	}
	access_nbtutils_Checker_set_skip_argument(self, key, false, 0);
	null_elem = ACCESS_NBTUTILS_ARRAY(self->pre, array, 892, 25)->null_elem;
	skey = ACCESS_NBTUTILS_KEY(self->pre, key, 893, 25);
	skey->flags &= ~(ACCESS_NBTUTILS_SK_SEARCHNULL | ACCESS_NBTUTILS_SK_ISNULL | ACCESS_NBTUTILS_SKIP_SENTINELS);
	if (null_elem && low_not_high == ((skey->flags & ACCESS_NBTUTILS_SK_BT_NULLS_FIRST) != 0))
		skey->flags |= ACCESS_NBTUTILS_SK_SEARCHNULL | ACCESS_NBTUTILS_SK_ISNULL;
	else if (low_not_high)
		skey->flags |= ACCESS_NBTUTILS_SK_BT_MINVAL;
	else
		skey->flags |= ACCESS_NBTUTILS_SK_BT_MAXVAL;
}

#ifdef DOLTGRES_TEST_ACCESS

/*
 * test_access_nbtutils_tests_a_scan_with_no_ordering_functions_leaves_its_function_as_it_was:
 * a scan whose keys hold no ordering functions answers none for a key's and
 * leaves its caller's function as the caller had it (bugs row 288).
 */
void
test_access_nbtutils_tests_a_scan_with_no_ordering_functions_leaves_its_function_as_it_was(void)
{
	AccessNbtutilsPreprocessed pre = {0};
	AccessNbtutilsChecker checker = {.pre = &pre};
	AccessNbtutilsFunction function = {.tag = ACCESS_NBTUTILS_FUNCTION_ORDER, .f0 = 0x5EED};

	STD_ASSERT(__LINE__, 5, !access_nbtutils_Checker_order_proc(&checker, 1, &function) && function.f0 == 0x5EED,
			   "a scan with no ordering functions answers none and leaves its function as it was");
}
#endif							/* DOLTGRES_TEST_ACCESS */
#endif
