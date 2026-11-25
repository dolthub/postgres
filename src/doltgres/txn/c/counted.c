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
 * Counted values: the values of keys that hold copies, a keyless table's rows
 * and its indexes' entries, whose value is how many copies the key holds as a
 * varint, then what each copy holds.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/varint.h"
#include "doltgres/std/num.h"
#ifdef DOLTGRES_TEST_TXN
#include "doltgres/std/test.h"
#endif
#include "doltgres/txn/counted.h"

#ifdef DOLTGRES_TEST_TXN
#define RUST_FILE "txn" STD_SEP "src" STD_SEP "counted.rs"
#endif

/* CORRUPT_COUNT is the message of a keyless value whose count is corrupt. */
static const char CORRUPT_COUNT[] = "a keyless row's count of copies is corrupt";

/*
 * txn_counted_counted_parts reads the count a value starts with and the tuple
 * after it, returning false when the value doesn't start with a count.
 */
static bool
txn_counted_counted_parts(const uint8 *value, size_t value_len, uint64 *count, StdBytes *tuple)
{
	size_t		at = 0;
	ChunksVarintMalformed malformed;

	if (!chunks_varint_decode(value, value_len, &at, count, &malformed))
		return false;
	tuple->ptr = value + at;
	tuple->len = value_len - at;
	return true;
}

/*
 * txn_counted_shifted fills a new vector with addresses whose offsets move
 * from a value's tuple after a prefix of one length to its tuple after a
 * prefix of another, saturating at the ends of an offset's range.
 */
static void
txn_counted_shifted(const chunks_TupleU32Locator_t *addresses, size_t addresses_len, uint32 from, uint32 to,
					StdVec *result)
{
	std_vec_with_capacity_Checked(result, addresses_len, sizeof(chunks_TupleU32Locator_t),
								  _Alignof(chunks_TupleU32Locator_t));
	for (size_t k = 0; k < addresses_len; k++)
	{
		chunks_TupleU32Locator_t moved = addresses[k];

		moved.f0 = std_saturating_add_u32(std_saturating_sub_u32(moved.f0, from), to);
		std_vec_push_Checked(result, &moved, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
	}
}

/*
 * txn_counted_recount_parts fills buffers with the counted value of a row's
 * tuple at another count, and the addresses of a value of a length moved to
 * that value, the buffers' old contents replaced.
 */
static void
txn_counted_recount_parts(size_t value_len, const uint8 *tuple, size_t tuple_len,
						  const chunks_TupleU32Locator_t *addresses, size_t addresses_len, uint64 count,
						  StdVec *out, StdVec *out_addresses)
{
	StdVec		moved;

	out->len = 0;
	chunks_varint_put_varint(out, count);
	std_vec_extend_from_slice_Checked(out, tuple, tuple_len, 1, 1);
	txn_counted_shifted(addresses, addresses_len, (uint32) std_saturating_sub_usize(value_len, tuple_len),
						(uint32) (out->len - tuple_len), &moved);
	std_vec_free_Checked(out_addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	*out_addresses = moved;
}

/*
 * txn_counted_keyless_parts returns how many copies of a row a keyless
 * table's value counts, and the row's tuple, refusing a count of zero, a
 * count not in its one form, or a value cut short before or after its count
 * as data corruption.
 */
bool
txn_counted_keyless_parts(const uint8 *value, size_t value_len, uint64 *result_0, StdBytes *result_1,
						  ChunksErrorError *error)
{
	if (txn_counted_counted_parts(value, value_len, result_0, result_1) && *result_0 != 0 && result_1->len != 0)
		return true;
	*error = chunks_error_Error_corrupt_RefStr(CORRUPT_COUNT, sizeof(CORRUPT_COUNT) - 1);
	return false;
}

/*
 * txn_counted_recount fills buffers with a counted value and its addresses
 * changed to count another number of copies, the buffers' old contents
 * replaced.
 */
bool
txn_counted_recount(const uint8 *value, size_t value_len, const chunks_TupleU32Locator_t *addresses,
					size_t addresses_len, uint64 count, StdVec *out, StdVec *out_addresses,
					ChunksErrorError *error)
{
	uint64		old_count;
	StdBytes	tuple;

	if (!txn_counted_keyless_parts(value, value_len, &old_count, &tuple, error))
		return false;
	txn_counted_recount_parts(value_len, tuple.ptr, tuple.len, addresses, addresses_len, count, out, out_addresses);
	return true;
}

/*
 * txn_counted_keyless_value returns the value of a row of a table without a
 * primary key: how many copies of the row the table holds, then its tuple.
 */
void
txn_counted_keyless_value(uint64 count, const uint8 *tuple, size_t tuple_len, StdVec *result)
{
	std_vec_with_capacity_Checked(result, std_saturating_add_usize(tuple_len, 2), 1, 1);
	chunks_varint_put_varint(result, count);
	std_vec_extend_from_slice_Checked(result, tuple, tuple_len, 1, 1);
}

/*
 * txn_counted_keyless_addresses returns where the value of a keyless row's
 * copies holds the addresses its tuple holds.
 */
void
txn_counted_keyless_addresses(uint64 count, const chunks_TupleU32Locator_t *addresses, size_t addresses_len,
							  StdVec *result)
{
	txn_counted_shifted(addresses, addresses_len, 0, (uint32) chunks_varint_varint_len(count), result);
}

#ifdef DOLTGRES_TEST_TXN

/*
 * test_txn_counted_tests_a_keyless_value_is_its_count_and_its_tuple_closure_0
 * is the test's refused: whether keyless_parts refuses a value as data
 * corruption.
 */
static bool
test_txn_counted_tests_a_keyless_value_is_its_count_and_its_tuple_closure_0(const uint8 *value, size_t value_len)
{
	uint64		count;
	StdBytes	tuple;
	ChunksErrorError error;
	bool		corrupt;

	if (txn_counted_keyless_parts(value, value_len, &count, &tuple, &error))
		return false;
	corrupt = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
	chunks_error_Error_drop(&error);
	return corrupt;
}

/* REFUSED is the test's refused. */
#define REFUSED test_txn_counted_tests_a_keyless_value_is_its_count_and_its_tuple_closure_0

/*
 * test_txn_counted_tests_a_keyless_value_is_its_count_and_its_tuple: a keyless
 * entry's value reads back as the count and the tuple it was written with,
 * and a count of zero, a count not in its shortest form, and a value cut
 * short before or after its count are each data corruption.
 */
void
test_txn_counted_tests_a_keyless_value_is_its_count_and_its_tuple(void)
{
	static const uint8 tuple[] = "a tuple";
	static const uint64 counts[] = {1, 2, 127, 128, PG_UINT64_MAX};
	static const uint8 long_zero[] = {0x82, 0x00, 'a', ' ', 't', 'u', 'p', 'l', 'e'};
	static const uint8 cut[] = {0x80};
	const size_t tuple_len = sizeof(tuple) - 1;
	StdVec		value;
	bool		all_refused;

	for (size_t i = 0; i < lengthof(counts); i++)
	{
		uint64		read;
		StdBytes	back;
		ChunksErrorError error;
		bool		matched = false;

		txn_counted_keyless_value(counts[i], tuple, tuple_len, &value);
		if (txn_counted_keyless_parts(value.ptr, value.len, &read, &back, &error))
			matched = read == counts[i] && back.len == tuple_len && memcmp(back.ptr, tuple, tuple_len) == 0;
		else
			chunks_error_Error_drop(&error);
		std_vec_free_Checked(&value, 1, 1);
		STD_ASSERT(240, 13, matched,
				   "matches!(keyless_parts(&value), Ok((read, back)) if read == count && back == tuple.as_slice())");
	}
	txn_counted_keyless_value(0, tuple, tuple_len, &value);
	all_refused = REFUSED(value.ptr, value.len);
	std_vec_free_Checked(&value, 1, 1);
	STD_ASSERT(243, 9, all_refused, "refused(&keyless_value(0, &tuple))");
	STD_ASSERT(244, 9, REFUSED(long_zero, sizeof(long_zero)), "refused(&[&[0x82, 0x00][..], &tuple[..]].concat())");
	txn_counted_keyless_value(2, tuple, 0, &value);
	all_refused = REFUSED(cut, 0) && REFUSED(value.ptr, value.len) && REFUSED(cut, sizeof(cut));
	std_vec_free_Checked(&value, 1, 1);
	STD_ASSERT(245, 9, all_refused, "refused(&[]) && refused(&keyless_value(2, &[])) && refused(&[0x80])");
}

#undef REFUSED

#endif							/* DOLTGRES_TEST_TXN */
