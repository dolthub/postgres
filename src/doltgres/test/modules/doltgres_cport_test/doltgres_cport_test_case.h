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
 * A C port test and its outcome's judgment, which the test modules' runner
 * in a backend and the frontend test binaries' runner share.
 */
#ifndef DOLTGRES_CPORT_TEST_CASE_H
#define DOLTGRES_CPORT_TEST_CASE_H

#include "doltgres/std/io.h"
#include "doltgres/std/unwind.h"

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

extern char *doltgres_cport_test_judged(const CportTest *test, bool passed, StdCaught *caught);
extern char *doltgres_cport_test_with_output(char *outcome, const StdOutputCapture *capture);

#endif							/* DOLTGRES_CPORT_TEST_CASE_H */
