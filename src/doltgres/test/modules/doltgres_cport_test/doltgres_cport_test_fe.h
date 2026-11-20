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
 * The frontend test binaries' runner (the port text's section 7): a
 * frontend configuration's tests run in a C executable, outside a backend,
 * each judged as the test modules' runner judges it, and printed one row
 * each of its name and outcome.
 */
#ifndef DOLTGRES_CPORT_TEST_FE_H
#define DOLTGRES_CPORT_TEST_FE_H

#include "doltgres_cport_test_case.h"

extern int	doltgres_cport_test_fe_run_all(const CportTest *tests, size_t len);

#endif							/* DOLTGRES_CPORT_TEST_FE_H */
