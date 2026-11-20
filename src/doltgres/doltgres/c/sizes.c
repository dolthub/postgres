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
 * tree's totals, and a TOAST relation's from its value map's.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/doltgres/sizes.h"
#include "doltgres/std/num.h"
#ifdef DOLTGRES_TEST_DOLTGRES
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "sizes.rs"

/*
 * doltgres_sizes_blocks returns a table's block count from its tree's
 * totals, its tuple id counter, and a page's usable bytes: the larger of its
 * heap pages, its rows packed uniformly at their mean heap bytes, and the
 * blocks its rows' ctids name, the counter's block plus one; a table whose
 * counter names no row has none. A mean below one byte counts as one.
 */
uint64
doltgres_sizes_blocks(const TxnBaseSizes *totals, uint64 counter, uint64 usable)
{
	uint64		named = counter > 1 ? (counter >> 16) + 1 : 0;
	uint64		per_page;

	if (totals->rows == 0)
		return named;
	per_page = Max(usable / Max(std_div_ceil_u64(totals->heap_bytes, totals->rows, RUST_FILE, 29, 48), 1), 1);
	return Max(std_div_ceil_u64(totals->rows, per_page, RUST_FILE, 30, 17), named);
}

/*
 * doltgres_sizes_toast_blocks returns a TOAST relation's block count from
 * its value map's totals and a page's usable bytes: a page holds as many
 * values as fit in it whole, and a value larger than a page takes the pages
 * its chunks fill, at the values' mean heap bytes.
 */
uint64
doltgres_sizes_toast_blocks(const TxnBaseSizes *totals, uint64 usable)
{
	uint64		mean;
	uint64		fit;

	if (totals->entries == 0)
		return 0;
	mean = Max(std_div_ceil_u64(totals->heap_bytes, totals->entries, RUST_FILE, 41, 34), 1);
	fit = usable / mean;
	if (fit == 0)
		return STD_MUL(u64, totals->entries, std_div_ceil_u64(mean, Max(usable, 1), RUST_FILE, 43, 36), 43, 14);
	return std_div_ceil_u64(totals->entries, fit, RUST_FILE, 44, 31);
}

#ifdef DOLTGRES_TEST_DOLTGRES

/* doltgres_sizes_tests_sized returns totals of rows of heap bytes. */
static TxnBaseSizes
doltgres_sizes_tests_sized(uint64 rows, uint64 heap_bytes)
{
	TxnBaseSizes sizes = {.rows = rows, .heap_bytes = heap_bytes};

	return sizes;
}

/*
 * test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole_closure_0
 * returns totals of values of one heap size.
 */
static TxnBaseSizes
test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole_closure_0(uint64 entries, uint64 each)
{
	TxnBaseSizes sizes = {.entries = entries, .heap_bytes = STD_MUL(u64, entries, each, 90, 77)};

	return sizes;
}

/*
 * test_doltgres_sizes_tests_a_tables_blocks_are_its_rows_packed_and_at_least_its_ctid_blocks
 * checks a table's blocks: its rows packed uniformly at their mean heap
 * bytes, at least one a page, and at least the blocks its ctids name.
 */
void
test_doltgres_sizes_tests_a_tables_blocks_are_its_rows_packed_and_at_least_its_ctid_blocks(void)
{
	uint64		usable = 8168;
	TxnBaseSizes sizes;

	sizes = doltgres_sizes_tests_sized(0, 0);
	STD_ASSERT_EQ_U64(64, 9, doltgres_sizes_blocks(&sizes, 1, usable), 0);
	sizes = doltgres_sizes_tests_sized(4, 4 * 2042);
	STD_ASSERT_EQ_U64(65, 9, doltgres_sizes_blocks(&sizes, 1, usable), 1);
	sizes = doltgres_sizes_tests_sized(5, 5 * 2042);
	STD_ASSERT_EQ_U64(66, 9, doltgres_sizes_blocks(&sizes, 1, usable), 2);
	sizes = doltgres_sizes_tests_sized(300, 300 * 1932);
	STD_ASSERT_EQ_U64(67, 9, doltgres_sizes_blocks(&sizes, 1, usable), 75);
	sizes = doltgres_sizes_tests_sized(3, 3 * 9000);
	STD_ASSERT_EQ_U64(68, 9, doltgres_sizes_blocks(&sizes, 1, usable), 3);
	sizes = doltgres_sizes_tests_sized(292, 292 * 28);
	STD_ASSERT_EQ_U64(69, 9, doltgres_sizes_blocks(&sizes, 1 << 16 | 1, usable), 2);
	sizes = doltgres_sizes_tests_sized(10, 10 * 28);
	STD_ASSERT_EQ_U64(70, 9, doltgres_sizes_blocks(&sizes, 5 << 16 | 1, usable), 6);
	sizes = doltgres_sizes_tests_sized(5000, 5000 * 44);
	STD_ASSERT_EQ_U64(71, 9, doltgres_sizes_blocks(&sizes, 1, usable / 2), 55);
	sizes = doltgres_sizes_tests_sized(1, 0);
	STD_ASSERT_EQ_U64(72, 9, doltgres_sizes_blocks(&sizes, 1, usable), 1);
	sizes = doltgres_sizes_tests_sized(usable + 1, 0);
	STD_ASSERT_EQ_U64(73, 9, doltgres_sizes_blocks(&sizes, 1, usable), 2);
}

/*
 * test_doltgres_sizes_tests_a_tables_delta_rows_count_their_ctid_blocks
 * checks that a table whose rows are all in the delta has the blocks its
 * counter names.
 */
void
test_doltgres_sizes_tests_a_tables_delta_rows_count_their_ctid_blocks(void)
{
	uint64		usable = 8168;
	TxnBaseSizes sizes = doltgres_sizes_tests_sized(0, 0);

	STD_ASSERT_EQ_U64(81, 9, doltgres_sizes_blocks(&sizes, 2, usable), 1);
	STD_ASSERT_EQ_U64(82, 9, doltgres_sizes_blocks(&sizes, 3 << 16 | 4, usable), 4);
}

/*
 * test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole
 * checks a TOAST relation's blocks: whole values to a page, and a value
 * larger than a page the pages its chunks fill.
 */
void
test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole(void)
{
	uint64		usable = 8168;
	TxnBaseSizes sizes;

	sizes = test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole_closure_0(0, 0);
	STD_ASSERT_EQ_U64(91, 9, doltgres_sizes_toast_blocks(&sizes, usable), 0);
	sizes = test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole_closure_0(50, 6560);
	STD_ASSERT_EQ_U64(92, 9, doltgres_sizes_toast_blocks(&sizes, usable), 50);
	sizes = test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole_closure_0(50, 2640);
	STD_ASSERT_EQ_U64(93, 9, doltgres_sizes_toast_blocks(&sizes, usable), 17);
	sizes = test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole_closure_0(300, 2192);
	STD_ASSERT_EQ_U64(94, 9, doltgres_sizes_toast_blocks(&sizes, usable), 100);
	sizes = test_doltgres_sizes_tests_a_toast_relations_blocks_are_its_values_packed_whole_closure_0(2, 20400);
	STD_ASSERT_EQ_U64(95, 9, doltgres_sizes_toast_blocks(&sizes, usable), 6);
}

#endif
