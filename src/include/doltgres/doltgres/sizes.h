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
 * A table's size for costs (doltgres' sizes.rs): its block count from its
 * tree's totals.
 */
#ifndef DOLTGRES_DOLTGRES_SIZES_H
#define DOLTGRES_DOLTGRES_SIZES_H

#include "doltgres/txn/base.h"

extern uint64 doltgres_sizes_blocks(const TxnBaseSizes *totals, uint64 counter, uint64 usable);
extern uint64 doltgres_sizes_toast_blocks(const TxnBaseSizes *totals, uint64 usable);

#ifdef DOLTGRES_TEST_DOLTGRES
extern void test_doltgres_sizes_tests_a_tables_blocks_are_its_rows_packed_and_at_least_its_ctid_blocks(void);
extern void test_doltgres_sizes_tests_a_tables_delta_rows_count_their_ctid_blocks(void);
extern void test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole(void);
#endif

#endif							/* DOLTGRES_DOLTGRES_SIZES_H */
