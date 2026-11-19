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
 * The C port's test modules' runner (the port text's section 13), which each
 * crate's module compiles: a test is run in its own internal subtransaction,
 * and a module's tests become one row each of its name and outcome.
 */
#ifndef DOLTGRES_CPORT_TEST_H
#define DOLTGRES_CPORT_TEST_H

#include "fmgr.h"

/*
 * CportTest is a test: its Rust path, its C function, whether it is a
 * #[should_panic] test, and the text its panic must contain when it names one
 * (expected = "..."), NULL when it doesn't.
 */
typedef struct CportTest
{
	const char *name;
	void		(*run) (void);
	bool		should_panic;
	const char *expected;
} CportTest;

extern Datum doltgres_cport_test_run_all(FunctionCallInfo fcinfo, const CportTest *tests, size_t len);

#endif							/* DOLTGRES_CPORT_TEST_H */
