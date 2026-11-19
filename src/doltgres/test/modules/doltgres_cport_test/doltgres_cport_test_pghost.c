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
 * The C port's pghost unit tests (the port text's section 13), pghost's
 * crate built with DOLTGRES_TEST_PGHOST as cargo builds its unit tests, run
 * in a backend: one row per test, in the Rust's order, with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/pghost/lib.h"
#include "doltgres/pghost/shared.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* CPORT_TESTS are pghost's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"shared::tests::a_place_is_a_kind_and_a_number", test_pghost_shared_tests_a_place_is_a_kind_and_a_number},
	{"shared::tests::the_holders_field_holds_every_process",
	test_pghost_shared_tests_the_holders_field_holds_every_process},
	{"tests::timestamps_convert_from_postgres_epoch", test_pghost_tests_timestamps_convert_from_postgres_epoch},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_pghost);

/* doltgres_cport_test_pghost runs pghost's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_pghost(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
