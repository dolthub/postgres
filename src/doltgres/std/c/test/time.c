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
 * The edge tests of std's Duration Debug (the port text's section 3):
 * fmt_decimal's units, rounding, carry past u64, precision, width,
 * alignment, and sign, each against Rust 1.99.1's own text, which a Rust
 * program printed into this table.
 */
#include "postgres.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/time.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* TimeDurationCase is a Duration Debug case: the duration, the placeholder's precision, width, alignment, and sign, and Rust's text. */
typedef struct TimeDurationCase
{
	StdDuration duration;
	bool		has_precision;
	uint16		precision;
	bool		has_width;
	uint16		width;
	StdFmtAlign align;
	bool		sign_plus;
	const char *expected;
} TimeDurationCase;

/* TIME_DURATION_CASES are Duration Debug's cases, each duration under each of eight placeholders. */
static const TimeDurationCase TIME_DURATION_CASES[] = {
	{{UINT64CONST(0), 0}, false, 0, false, 0, 3, false, "0ns"},
	{{UINT64CONST(0), 0}, true, 0, false, 0, 3, false, "0ns"},
	{{UINT64CONST(0), 0}, true, 2, false, 0, 3, false, "0.00ns"},
	{{UINT64CONST(0), 0}, true, 9, false, 0, 3, false, "0.000000000ns"},
	{{UINT64CONST(0), 0}, false, 0, true, 12, 3, false, "0ns         "},
	{{UINT64CONST(0), 0}, true, 3, true, 15, 1, false, "        0.000ns"},
	{{UINT64CONST(0), 0}, false, 0, true, 20, 2, false, "        0ns         "},
	{{UINT64CONST(0), 0}, false, 0, false, 0, 3, true, "+0ns"},
	{{UINT64CONST(0), 1}, false, 0, false, 0, 3, false, "1ns"},
	{{UINT64CONST(0), 1}, true, 0, false, 0, 3, false, "1ns"},
	{{UINT64CONST(0), 1}, true, 2, false, 0, 3, false, "1.00ns"},
	{{UINT64CONST(0), 1}, true, 9, false, 0, 3, false, "1.000000000ns"},
	{{UINT64CONST(0), 1}, false, 0, true, 12, 3, false, "1ns         "},
	{{UINT64CONST(0), 1}, true, 3, true, 15, 1, false, "        1.000ns"},
	{{UINT64CONST(0), 1}, false, 0, true, 20, 2, false, "        1ns         "},
	{{UINT64CONST(0), 1}, false, 0, false, 0, 3, true, "+1ns"},
	{{UINT64CONST(0), 999}, false, 0, false, 0, 3, false, "999ns"},
	{{UINT64CONST(0), 999}, true, 0, false, 0, 3, false, "999ns"},
	{{UINT64CONST(0), 999}, true, 2, false, 0, 3, false, "999.00ns"},
	{{UINT64CONST(0), 999}, true, 9, false, 0, 3, false, "999.000000000ns"},
	{{UINT64CONST(0), 999}, false, 0, true, 12, 3, false, "999ns       "},
	{{UINT64CONST(0), 999}, true, 3, true, 15, 1, false, "      999.000ns"},
	{{UINT64CONST(0), 999}, false, 0, true, 20, 2, false, "       999ns        "},
	{{UINT64CONST(0), 999}, false, 0, false, 0, 3, true, "+999ns"},
	{{UINT64CONST(0), 1000}, false, 0, false, 0, 3, false, "1\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1000}, true, 0, false, 0, 3, false, "1\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1000}, true, 2, false, 0, 3, false, "1.00\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1000}, true, 9, false, 0, 3, false, "1.000000000\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1000}, false, 0, true, 12, 3, false, "1\xc2" "\xb5" "s         "},
	{{UINT64CONST(0), 1000}, true, 3, true, 15, 1, false, "        1.000\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1000}, false, 0, true, 20, 2, false, "        1\xc2" "\xb5" "s         "},
	{{UINT64CONST(0), 1000}, false, 0, false, 0, 3, true, "+1\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1500}, false, 0, false, 0, 3, false, "1.5\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1500}, true, 0, false, 0, 3, false, "2\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1500}, true, 2, false, 0, 3, false, "1.50\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1500}, true, 9, false, 0, 3, false, "1.500000000\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1500}, false, 0, true, 12, 3, false, "1.5\xc2" "\xb5" "s       "},
	{{UINT64CONST(0), 1500}, true, 3, true, 15, 1, false, "        1.500\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1500}, false, 0, true, 20, 2, false, "       1.5\xc2" "\xb5" "s        "},
	{{UINT64CONST(0), 1500}, false, 0, false, 0, 3, true, "+1.5\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 999999}, false, 0, false, 0, 3, false, "999.999\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 999999}, true, 0, false, 0, 3, false, "1000\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 999999}, true, 2, false, 0, 3, false, "1000.00\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 999999}, true, 9, false, 0, 3, false, "999.999000000\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 999999}, false, 0, true, 12, 3, false, "999.999\xc2" "\xb5" "s   "},
	{{UINT64CONST(0), 999999}, true, 3, true, 15, 1, false, "      999.999\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 999999}, false, 0, true, 20, 2, false, "     999.999\xc2" "\xb5" "s      "},
	{{UINT64CONST(0), 999999}, false, 0, false, 0, 3, true, "+999.999\xc2" "\xb5" "s"},
	{{UINT64CONST(0), 1000000}, false, 0, false, 0, 3, false, "1ms"},
	{{UINT64CONST(0), 1000000}, true, 0, false, 0, 3, false, "1ms"},
	{{UINT64CONST(0), 1000000}, true, 2, false, 0, 3, false, "1.00ms"},
	{{UINT64CONST(0), 1000000}, true, 9, false, 0, 3, false, "1.000000000ms"},
	{{UINT64CONST(0), 1000000}, false, 0, true, 12, 3, false, "1ms         "},
	{{UINT64CONST(0), 1000000}, true, 3, true, 15, 1, false, "        1.000ms"},
	{{UINT64CONST(0), 1000000}, false, 0, true, 20, 2, false, "        1ms         "},
	{{UINT64CONST(0), 1000000}, false, 0, false, 0, 3, true, "+1ms"},
	{{UINT64CONST(0), 1234567}, false, 0, false, 0, 3, false, "1.234567ms"},
	{{UINT64CONST(0), 1234567}, true, 0, false, 0, 3, false, "1ms"},
	{{UINT64CONST(0), 1234567}, true, 2, false, 0, 3, false, "1.23ms"},
	{{UINT64CONST(0), 1234567}, true, 9, false, 0, 3, false, "1.234567000ms"},
	{{UINT64CONST(0), 1234567}, false, 0, true, 12, 3, false, "1.234567ms  "},
	{{UINT64CONST(0), 1234567}, true, 3, true, 15, 1, false, "        1.235ms"},
	{{UINT64CONST(0), 1234567}, false, 0, true, 20, 2, false, "     1.234567ms     "},
	{{UINT64CONST(0), 1234567}, false, 0, false, 0, 3, true, "+1.234567ms"},
	{{UINT64CONST(0), 999999999}, false, 0, false, 0, 3, false, "999.999999ms"},
	{{UINT64CONST(0), 999999999}, true, 0, false, 0, 3, false, "1000ms"},
	{{UINT64CONST(0), 999999999}, true, 2, false, 0, 3, false, "1000.00ms"},
	{{UINT64CONST(0), 999999999}, true, 9, false, 0, 3, false, "999.999999000ms"},
	{{UINT64CONST(0), 999999999}, false, 0, true, 12, 3, false, "999.999999ms"},
	{{UINT64CONST(0), 999999999}, true, 3, true, 15, 1, false, "     1000.000ms"},
	{{UINT64CONST(0), 999999999}, false, 0, true, 20, 2, false, "    999.999999ms    "},
	{{UINT64CONST(0), 999999999}, false, 0, false, 0, 3, true, "+999.999999ms"},
	{{UINT64CONST(1), 0}, false, 0, false, 0, 3, false, "1s"},
	{{UINT64CONST(1), 0}, true, 0, false, 0, 3, false, "1s"},
	{{UINT64CONST(1), 0}, true, 2, false, 0, 3, false, "1.00s"},
	{{UINT64CONST(1), 0}, true, 9, false, 0, 3, false, "1.000000000s"},
	{{UINT64CONST(1), 0}, false, 0, true, 12, 3, false, "1s          "},
	{{UINT64CONST(1), 0}, true, 3, true, 15, 1, false, "         1.000s"},
	{{UINT64CONST(1), 0}, false, 0, true, 20, 2, false, "         1s         "},
	{{UINT64CONST(1), 0}, false, 0, false, 0, 3, true, "+1s"},
	{{UINT64CONST(1), 5}, false, 0, false, 0, 3, false, "1.000000005s"},
	{{UINT64CONST(1), 5}, true, 0, false, 0, 3, false, "1s"},
	{{UINT64CONST(1), 5}, true, 2, false, 0, 3, false, "1.00s"},
	{{UINT64CONST(1), 5}, true, 9, false, 0, 3, false, "1.000000005s"},
	{{UINT64CONST(1), 5}, false, 0, true, 12, 3, false, "1.000000005s"},
	{{UINT64CONST(1), 5}, true, 3, true, 15, 1, false, "         1.000s"},
	{{UINT64CONST(1), 5}, false, 0, true, 20, 2, false, "    1.000000005s    "},
	{{UINT64CONST(1), 5}, false, 0, false, 0, 3, true, "+1.000000005s"},
	{{UINT64CONST(1), 500000000}, false, 0, false, 0, 3, false, "1.5s"},
	{{UINT64CONST(1), 500000000}, true, 0, false, 0, 3, false, "2s"},
	{{UINT64CONST(1), 500000000}, true, 2, false, 0, 3, false, "1.50s"},
	{{UINT64CONST(1), 500000000}, true, 9, false, 0, 3, false, "1.500000000s"},
	{{UINT64CONST(1), 500000000}, false, 0, true, 12, 3, false, "1.5s        "},
	{{UINT64CONST(1), 500000000}, true, 3, true, 15, 1, false, "         1.500s"},
	{{UINT64CONST(1), 500000000}, false, 0, true, 20, 2, false, "        1.5s        "},
	{{UINT64CONST(1), 500000000}, false, 0, false, 0, 3, true, "+1.5s"},
	{{UINT64CONST(2), 999999999}, false, 0, false, 0, 3, false, "2.999999999s"},
	{{UINT64CONST(2), 999999999}, true, 0, false, 0, 3, false, "3s"},
	{{UINT64CONST(2), 999999999}, true, 2, false, 0, 3, false, "3.00s"},
	{{UINT64CONST(2), 999999999}, true, 9, false, 0, 3, false, "2.999999999s"},
	{{UINT64CONST(2), 999999999}, false, 0, true, 12, 3, false, "2.999999999s"},
	{{UINT64CONST(2), 999999999}, true, 3, true, 15, 1, false, "         3.000s"},
	{{UINT64CONST(2), 999999999}, false, 0, true, 20, 2, false, "    2.999999999s    "},
	{{UINT64CONST(2), 999999999}, false, 0, false, 0, 3, true, "+2.999999999s"},
	{{UINT64CONST(59), 123456789}, false, 0, false, 0, 3, false, "59.123456789s"},
	{{UINT64CONST(59), 123456789}, true, 0, false, 0, 3, false, "59s"},
	{{UINT64CONST(59), 123456789}, true, 2, false, 0, 3, false, "59.12s"},
	{{UINT64CONST(59), 123456789}, true, 9, false, 0, 3, false, "59.123456789s"},
	{{UINT64CONST(59), 123456789}, false, 0, true, 12, 3, false, "59.123456789s"},
	{{UINT64CONST(59), 123456789}, true, 3, true, 15, 1, false, "        59.123s"},
	{{UINT64CONST(59), 123456789}, false, 0, true, 20, 2, false, "   59.123456789s    "},
	{{UINT64CONST(59), 123456789}, false, 0, false, 0, 3, true, "+59.123456789s"},
	{{UINT64CONST(18446744073709551615), 999999999}, false, 0, false, 0, 3, false, "18446744073709551615.999999999s"},
	{{UINT64CONST(18446744073709551615), 999999999}, true, 0, false, 0, 3, false, "18446744073709551616s"},
	{{UINT64CONST(18446744073709551615), 999999999}, true, 2, false, 0, 3, false, "18446744073709551616.00s"},
	{{UINT64CONST(18446744073709551615), 999999999}, true, 9, false, 0, 3, false, "18446744073709551615.999999999s"},
	{{UINT64CONST(18446744073709551615), 999999999}, false, 0, true, 12, 3, false, "18446744073709551615.999999999s"},
	{{UINT64CONST(18446744073709551615), 999999999}, true, 3, true, 15, 1, false, "18446744073709551616.000s"},
	{{UINT64CONST(18446744073709551615), 999999999}, false, 0, true, 20, 2, false, "18446744073709551615.999999999s"},
	{{UINT64CONST(18446744073709551615), 999999999}, false, 0, false, 0, 3, true, "+18446744073709551615.999999999s"},
	{{UINT64CONST(18446744073709551615), 0}, false, 0, false, 0, 3, false, "18446744073709551615s"},
	{{UINT64CONST(18446744073709551615), 0}, true, 0, false, 0, 3, false, "18446744073709551615s"},
	{{UINT64CONST(18446744073709551615), 0}, true, 2, false, 0, 3, false, "18446744073709551615.00s"},
	{{UINT64CONST(18446744073709551615), 0}, true, 9, false, 0, 3, false, "18446744073709551615.000000000s"},
	{{UINT64CONST(18446744073709551615), 0}, false, 0, true, 12, 3, false, "18446744073709551615s"},
	{{UINT64CONST(18446744073709551615), 0}, true, 3, true, 15, 1, false, "18446744073709551615.000s"},
	{{UINT64CONST(18446744073709551615), 0}, false, 0, true, 20, 2, false, "18446744073709551615s"},
	{{UINT64CONST(18446744073709551615), 0}, false, 0, false, 0, 3, true, "+18446744073709551615s"},
	{{UINT64CONST(10), 5000000}, false, 0, false, 0, 3, false, "10.005s"},
	{{UINT64CONST(10), 5000000}, true, 0, false, 0, 3, false, "10s"},
	{{UINT64CONST(10), 5000000}, true, 2, false, 0, 3, false, "10.00s"},
	{{UINT64CONST(10), 5000000}, true, 9, false, 0, 3, false, "10.005000000s"},
	{{UINT64CONST(10), 5000000}, false, 0, true, 12, 3, false, "10.005s     "},
	{{UINT64CONST(10), 5000000}, true, 3, true, 15, 1, false, "        10.005s"},
	{{UINT64CONST(10), 5000000}, false, 0, true, 20, 2, false, "      10.005s       "},
	{{UINT64CONST(10), 5000000}, false, 0, false, 0, 3, true, "+10.005s"},
};

/* test_std_time_tests_duration_debug_matches_rust checks Duration's Debug against Rust's text under each placeholder. */
void
test_std_time_tests_duration_debug_matches_rust(void)
{
	for (size_t i = 0; i < lengthof(TIME_DURATION_CASES); i++)
	{
		const TimeDurationCase *c = &TIME_DURATION_CASES[i];
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&c->duration, std_Duration_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString text;
		bool		same;

		pieces[0].spec.has_precision = c->has_precision;
		pieces[0].spec.precision = c->precision;
		pieces[0].spec.has_width = c->has_width;
		pieces[0].spec.width = c->width;
		pieces[0].spec.align = c->align;
		pieces[0].spec.sign_plus = c->sign_plus;
		std_fmt_format_Checked(&text, &arguments);
		same = text.len == strlen(c->expected) && memcmp(text.ptr, c->expected, text.len) == 0;

		std_vec_free_Checked(&text, 1, 1);
		if (!same)
			std_panic(__FILE__, __LINE__, 0, "Duration's Debug differs from Rust's",
					  sizeof("Duration's Debug differs from Rust's") - 1);
	}
}
