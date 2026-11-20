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
 * The test-only forms of std::env and std::process the frontend
 * configuration's tests call beside test_fs.h's std::fs, which the frontend
 * test binaries' std/c/test/fe_fs.c gives over Postgres' frontend port layer
 * (the port text's section 7): the shipped C never includes this header.
 */
#ifndef DOLTGRES_STD_TEST_FE_H
#define DOLTGRES_STD_TEST_FE_H

#include "doltgres/std/path.h"

extern StdPathBuf std_env_temp_dir(void);
extern uint32 std_process_id(void);
extern void test_std_fe_fs_tests_a_directory_is_made_written_in_and_removed(void);

#endif							/* DOLTGRES_STD_TEST_FE_H */
