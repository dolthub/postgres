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
 * The C port of alloctests' str.rs tests of the str forms no crate's test
 * reaches, at Rust 1.99.1 (the port text's section 13).
 */
#ifndef DOLTGRES_STD_TEST_STR_H
#define DOLTGRES_STD_TEST_STR_H

extern void test_alloctests_str_utf8_char_counts(void);
extern void test_alloctests_str_slice_index_rangefrom_len_index_fail(void);
extern void test_alloctests_str_slice_index_rangeto_len_index_fail(void);
extern void test_alloctests_str_slice_index_range_len_len_index_fail(void);
extern void test_alloctests_str_slice_index_range_neg_width_index_fail(void);
extern void test_alloctests_str_slice_index_boundary_range_1_index_fail(void);
extern void test_alloctests_str_slice_index_boundary_range_2_index_fail(void);
extern void test_alloctests_str_slice_index_boundary_rangefrom_index_fail(void);
extern void test_alloctests_str_slice_index_boundary_rangeto_index_fail(void);

#endif							/* DOLTGRES_STD_TEST_STR_H */
