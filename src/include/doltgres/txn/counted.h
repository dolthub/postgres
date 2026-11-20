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
 * Counted values (txn's counted.rs): the values of keys that hold copies, a
 * keyless table's rows and its indexes' entries, whose value is how many
 * copies the key holds, then what each copy holds.
 */
#ifndef DOLTGRES_TXN_COUNTED_H
#define DOLTGRES_TXN_COUNTED_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

extern bool txn_counted_keyless_parts(const uint8 *value, size_t value_len, uint64 *result_0, StdBytes *result_1,
									  ChunksErrorError *error);
extern bool txn_counted_recount(const uint8 *value, size_t value_len, const chunks_TupleU32Locator_t *addresses,
								size_t addresses_len, uint64 count, StdVec *out, StdVec *out_addresses,
								ChunksErrorError *error);
extern void txn_counted_keyless_value(uint64 count, const uint8 *tuple, size_t tuple_len, StdVec *result);
extern void txn_counted_keyless_addresses(uint64 count, const chunks_TupleU32Locator_t *addresses,
										  size_t addresses_len, StdVec *result);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_counted_tests_a_keyless_value_is_its_count_and_its_tuple(void);
#endif

#endif							/* DOLTGRES_TXN_COUNTED_H */
