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
 * The frontend test binaries' runner (the port text's section 7): each test
 * of a frontend configuration runs with the test modules' judgment, without
 * a subtransaction, since a frontend has no transaction, and its name and
 * outcome are printed as one row, a tab between them and the outcome's
 * backslashes and newlines escaped, which the binary's TAP test reads.
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/io.h"
#include "doltgres/std/test.h"
#include "doltgres/std/vec.h"
#include "doltgres_cport_test_fe.h"

/*
 * std_io__print is std's _print under libtest's output capture: the
 * formatted text written into the running test's capture. The runner
 * captures every test's output, so a print outside it ends the binary.
 */
void
std_io__print(const StdFmtArguments *args)
{
	if (!std_io_print_to_buffer_if_capture_used(args))
		pg_fatal("a C port test printed outside its runner");
}

/* std_io__eprint is std's _eprint under libtest's output capture, which takes stderr's output as it takes stdout's. */
void
std_io__eprint(const StdFmtArguments *args)
{
	if (!std_io_print_to_buffer_if_capture_used(args))
		pg_fatal("a C port test printed outside its runner");
}

/* doltgres_cport_test_fe_run runs a test and returns its outcome, with its captured output after a failure's. */
static char *
doltgres_cport_test_fe_run(const CportTest *test)
{
	StdOutputCapture capture = {STD_RW_LOCK_NEW};
	StdUnwind	unwind;
	StdCaught	caught;
	bool		passed = false;
	char	   *outcome;

	std_vec_new(&capture.buf, 1, 1);
	(void) std_io_set_output_capture(&capture);
	STD_UNWIND_PUSH_CATCHING(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		test->run();
		std_unwind_pop(&unwind);
		passed = true;
	}
	else
		caught = std_unwind_end(&unwind);
	outcome = doltgres_cport_test_judged(test, passed, &caught);
	(void) std_io_set_output_capture(NULL);
	outcome = doltgres_cport_test_with_output(outcome, &capture);
	std_vec_free_Checked(&capture.buf, 1, 1);
	return outcome;
}

/* doltgres_cport_test_fe_printed prints an outcome with its backslashes and newlines escaped. */
static void
doltgres_cport_test_fe_printed(const char *outcome)
{
	for (const char *c = outcome; *c != '\0'; c++)
	{
		if (*c == '\\')
			fputs("\\\\", stdout);
		else if (*c == '\n')
			fputs("\\n", stdout);
		else
			fputc(*c, stdout);
	}
}

/*
 * doltgres_cport_test_fe_run_all runs a binary's tests in order and prints
 * one row of each test's name and outcome, returning the binary's exit
 * status: 0, whatever the outcomes, which the TAP test judges.
 */
int
doltgres_cport_test_fe_run_all(const CportTest *tests, size_t len)
{
	for (size_t i = 0; i < len; i++)
	{
		char	   *outcome = doltgres_cport_test_fe_run(&tests[i]);

		fputs(tests[i].name, stdout);
		fputc('\t', stdout);
		doltgres_cport_test_fe_printed(outcome);
		fputc('\n', stdout);
		pfree(outcome);
	}
	fflush(stdout);
	return 0;
}
