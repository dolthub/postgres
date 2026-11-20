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
 * The test of std's C forms of CStr: its Debug against the text Rust's
 * CStr Debug prints for each case, a table a Rust program printed.
 */
#include "postgres.h"

#include "doltgres/std/c_str.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* STD_C_STR_TESTS_DEBUG is each case's bytes and the Debug Rust's CStr prints for it. */
static const struct
{
	const char *bytes;
	const char *printed;
}			STD_C_STR_TESTS_DEBUG[] = {
	{"/datadir", "\"/datadir\""},
	{"it's", "\"it\\'s\""},
	{"tab\there", "\"tab\\there\""},
	{"quote\"back\\", "\"quote\\\"back\\\\\""},
	{"\x01\x7f\xff", "\"\\x01\\x7f\\xff\""},
	{"line\r\nend", "\"line\\r\\nend\""},
};

/* test_std_c_str_tests_a_cstrs_debug_escapes_as_rusts checks CStr's Debug against Rust's for each case. */
void
test_std_c_str_tests_a_cstrs_debug_escapes_as_rusts(void)
{
	for (size_t k = 0; k < lengthof(STD_C_STR_TESTS_DEBUG); k++)
	{
		StdString	printed;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{STD_C_STR_TESTS_DEBUG[k].bytes, std_CStr_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		const char *want = STD_C_STR_TESTS_DEBUG[k].printed;

		std_fmt_format_Checked(&printed, &arguments);
		if (printed.len != strlen(want) || memcmp(printed.ptr, want, printed.len) != 0)
			std_panic(__FILE__, __LINE__, 0, "a CStr's Debug differs from Rust's",
					  sizeof("a CStr's Debug differs from Rust's") - 1);
		std_String_drop(&printed);
	}
}
