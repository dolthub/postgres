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
 * The C port of coretests' fmt/builders.rs tests of DebugStruct, DebugTuple,
 * and DebugList at Rust 1.99.1 (the port text's section 13).
 */
#ifndef DOLTGRES_STD_TEST_FMT_H
#define DOLTGRES_STD_TEST_FMT_H

extern void test_coretests_fmt_builders_debug_struct_test_empty(void);
extern void test_coretests_fmt_builders_debug_struct_test_single(void);
extern void test_coretests_fmt_builders_debug_struct_test_multiple(void);
extern void test_coretests_fmt_builders_debug_struct_test_nested(void);
extern void test_coretests_fmt_builders_debug_struct_test_empty_non_exhaustive(void);
extern void test_coretests_fmt_builders_debug_struct_test_multiple_and_non_exhaustive(void);
extern void test_coretests_fmt_builders_debug_struct_test_nested_non_exhaustive(void);
extern void test_coretests_fmt_builders_debug_tuple_test_empty(void);
extern void test_coretests_fmt_builders_debug_tuple_test_single(void);
extern void test_coretests_fmt_builders_debug_tuple_test_multiple(void);
extern void test_coretests_fmt_builders_debug_tuple_test_nested(void);
extern void test_coretests_fmt_builders_debug_list_test_empty(void);
extern void test_coretests_fmt_builders_debug_list_test_single(void);
extern void test_coretests_fmt_builders_debug_list_test_multiple(void);
extern void test_coretests_fmt_builders_debug_list_test_nested(void);

#endif							/* DOLTGRES_STD_TEST_FMT_H */
