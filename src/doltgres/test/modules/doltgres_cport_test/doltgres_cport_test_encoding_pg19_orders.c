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
 * The C port's test of encoding's tests/pg19_orders.rs (the port text's
 * section 13), encoding built without its test switch as cargo builds an
 * integration test, run in a backend: one row per test with its outcome.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

extern void test_encoding_pg19_orders_keys_sort_as_postgres_sorts_values(void);

/* CPORT_TESTS are pg19_orders.rs's tests, in the order the Rust harness runs them, sorted by name. */
static const CportTest CPORT_TESTS[] = {
	{"keys_sort_as_postgres_sorts_values", test_encoding_pg19_orders_keys_sort_as_postgres_sorts_values},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_encoding_pg19_orders);

/* doltgres_cport_test_encoding_pg19_orders runs pg19_orders.rs's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_encoding_pg19_orders(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
