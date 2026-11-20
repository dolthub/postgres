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
 * The judgment of a C port test's outcome, as libtest's calc_result words
 * it, which the test modules' runner in a backend and the frontend test
 * binaries' runner share.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/fmt.h"
#include "doltgres/std/vec.h"
#include "doltgres_cport_test_case.h"

/*
 * doltgres_cport_test_payload_text is the text libtest reports a panic's
 * payload by: a &str's or a String's, or "Box<dyn Any>".
 */
static char *
doltgres_cport_test_payload_text(const StdCaught *caught)
{
	if (caught->payload == STD_PAYLOAD_STR)
	{
		const StdPayloadStr *s = caught->box;

		return pnstrdup(s->ptr, s->len);
	}
	if (caught->payload == STD_PAYLOAD_STRING)
	{
		const StdString *s = caught->box;

		return pnstrdup(s->ptr, s->len);
	}
	return pstrdup("Box<dyn Any>");
}

/*
 * doltgres_cport_test_debug_message is a should_panic failure's message as
 * libtest formats it: the lead, the panic's text in str's Debug when there is
 * one, then the expected substring's label and its text in str's Debug.
 */
static char *
doltgres_cport_test_debug_message(const char *lead, const char *message, const char *expected)
{
	StdStr		panic = {message, 0};
	StdStr		want = {expected, strlen(expected)};
	StdFmtPiece with_text[] = {STD_FMT_LIT(""), STD_FMT_ARG(0), STD_FMT_LIT("\n expected substring: "),
	STD_FMT_ARG(1)};
	StdFmtPiece without_text[] = {STD_FMT_LIT(""), STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&panic, std_str_Debug_fmt}, {&want, std_str_Debug_fmt}};
	StdFmtArguments arguments;
	StdString	s;
	char	   *out;

	if (message != NULL)
		panic.len = strlen(message);
	with_text[0].literal = lead;
	with_text[0].literal_len = strlen(lead);
	without_text[0].literal = lead;
	without_text[0].literal_len = strlen(lead);
	arguments.pieces = message == NULL ? without_text : with_text;
	arguments.pieces_len = message == NULL ? lengthof(without_text) : lengthof(with_text);
	arguments.args = args;
	std_fmt_format_Checked(&s, &arguments);
	out = pnstrdup(s.ptr, s.len);
	std_vec_free_Checked(&s, 1, 1);
	return out;
}

/*
 * doltgres_cport_test_judged returns a test's outcome from whether it
 * returned and, when it panicked, what it panicked with, which it drops:
 * "ok", libtest's message for a #[should_panic] test that failed, or the
 * panic's message.
 */
char *
doltgres_cport_test_judged(const CportTest *test, bool passed, StdCaught *caught)
{
	char	   *outcome;
	char	   *text;

	if (passed)
		return test->should_panic ? pstrdup("test did not panic as expected") : pstrdup("ok");
	text = doltgres_cport_test_payload_text(caught);

	if (!test->should_panic)
		outcome = psprintf("panicked: %s", text);
	else if (test->expected == NULL)
		outcome = pstrdup("ok");
	else if (caught->payload != STD_PAYLOAD_STR && caught->payload != STD_PAYLOAD_STRING)
		outcome = doltgres_cport_test_debug_message("expected panic with string value,\n found non-string value\n"
													"     expected substring: ",
													NULL, test->expected);
	else if (strstr(text, test->expected) != NULL)
		outcome = pstrdup("ok");
	else
		outcome = doltgres_cport_test_debug_message("panic did not contain expected string\n      panic message: ",
													text, test->expected);
	std_Caught_drop(caught);
	return outcome;
}

/* doltgres_cport_test_with_output returns an outcome with the test's captured output after it when it failed and printed. */
char *
doltgres_cport_test_with_output(char *outcome, const StdOutputCapture *capture)
{
	if (strcmp(outcome, "ok") == 0 || capture->buf.len == 0)
		return outcome;
	return psprintf("%s\n---- stdout ----\n%.*s", outcome, (int) capture->buf.len, (const char *) capture->buf.ptr);
}
