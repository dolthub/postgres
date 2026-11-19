// Copyright 2026 Dolthub, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use harness::oid::*;
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_advisory_locks() {
    run_scripts(&[
        ScriptTest {
            name: "basic lock tests",
            set_up_script: &[
                "CREATE USER user1 PASSWORD 'password';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock(1)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_lock(2)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_lock(1)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "user1",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_lock(2)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "user1",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(1)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(2)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(3)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "you don't own a lock of type ExclusiveLock", ..E }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "advisory locks are reentrant",
            set_up_script: &[
                "CREATE USER user1 PASSWORD 'password';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_lock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_lock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "user1",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_lock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "user1",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_lock(10)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "user1",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_advisory_locks_2() {
    run_scripts(&[
        ScriptTest {
            name: "transaction advisory locks",
            set_up_script: &[
                "CREATE TABLE lock_commit_test (pk INT PRIMARY KEY)",
                "SELECT DOLT_BRANCH('lock-test-branch')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(20)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(20)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(20)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(20)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_try_advisory_xact_lock(21)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_xact_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(21)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(21)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(21)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(42)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_unlock(42)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "you don't own a lock of type ExclusiveLock", ..E }],
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(42)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(42)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(42)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_lock(43)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(43)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_unlock(43)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_unlock(43)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "you don't own a lock of type ExclusiveLock", ..E }],
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(43)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(43)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(43)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(22)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_xact_lock(22)",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client C */ SELECT pg_try_advisory_lock(22)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client C */ SELECT pg_advisory_unlock(22)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_try_advisory_xact_lock(23)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_xact_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(23)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(23)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(24)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO lock_commit_test VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT DOLT_COMMIT('-Am', 'lock lifecycle test')",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(24)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(24)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, and DOLT_COMMIT leaves the transaction block open, so this expectation follows Postgres' warning.
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25001", message: "there is already a transaction in progress", ..N }],
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this multi-client script, so this expectation follows Postgres' behavior.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(25)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ SELECT DOLT_CHECKOUT('lock-test-branch')",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(25)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(25)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(25)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "transaction locks release across session reuse",
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(30)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(30)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(30)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(31)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_try_advisory_lock(31)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(31)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(32)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_xact_lock(32)",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT pg_try_advisory_lock(32)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT pg_advisory_unlock(32)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "advisory locks release when client disconnects",
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_lock(40)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_xact_lock(41)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_lock(40)",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT pg_advisory_xact_lock(41)",
                    flow: Flow::Exec,
                    client: "C",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DISCONNECT",
                    flow: Flow::Exec,
                    client: "A",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(40)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    flow: Flow::Exec,
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ SELECT pg_try_advisory_lock(40)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ SELECT pg_advisory_unlock(40)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ SELECT pg_try_advisory_lock(41)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ SELECT pg_advisory_unlock(41)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "D",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_lock_table() {
    run_scripts(&[
        ScriptTest {
            name: "LOCK checks the transaction, relations, and privileges",
            set_up_script: &[
                "CREATE TABLE lk (id INT PRIMARY KEY);",
                "CREATE VIEW lk_view AS SELECT * FROM lk;",
                "CREATE SEQUENCE lk_seq;",
                "CREATE ROLE regress_lk_user;",
                "GRANT SELECT ON lk TO regress_lk_user;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "LOCK TABLE lk;",
                    expected: Expected::Error(Diagnostic { code: "25P01", message: "LOCK TABLE can only be used in transaction blocks", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE lk IN ACCESS SHARE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK lk IN ROW EXCLUSIVE MODE NOWAIT;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE lk, lk_view IN ACCESS EXCLUSIVE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE pg_class IN ACCESS SHARE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE nope;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"nope\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE lk_pkey;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot lock relation \"lk_pkey\"", detail: "This operation is not supported for indexes.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE lk_seq;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot lock relation \"lk_seq\"", detail: "This operation is not supported for sequences.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_lk_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE lk IN ACCESS SHARE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE lk IN ROW EXCLUSIVE MODE;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table lk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_lock_status() {
    run_scripts(&[
        ScriptTest {
            name: "pg_locks shows a session's own locks, and advisory locks take, share, and release",
            set_up_script: &[
                "CREATE TABLE lf (a INT PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT locktype, mode, granted FROM pg_locks WHERE pid = pg_backend_pid() AND locktype <> 'relation' ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("mode", TEXT), Column("granted", BOOL)],
                        rows: &[
                            &[T("virtualxid"), T("ExclusiveLock"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, mode, granted, virtualxid = virtualtransaction AS own FROM pg_locks WHERE pid = pg_backend_pid() AND locktype <> 'relation' ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("mode", TEXT), Column("granted", BOOL), Column("own", BOOL)],
                        rows: &[
                            &[T("virtualxid"), T("ExclusiveLock"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pid = pg_backend_pid() AS own FROM pg_locks WHERE locktype = 'virtualxid' AND virtualxid = virtualtransaction AND pid = pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("own", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lf VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, mode, granted, transactionid IS NOT NULL AS own FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'transactionid';",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("mode", TEXT), Column("granted", BOOL), Column("own", BOOL)],
                        rows: &[
                            &[T("transactionid"), T("ExclusiveLock"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass, mode, granted, database = (SELECT oid FROM pg_database WHERE datname = current_database()) AS db FROM pg_locks WHERE pid = pg_backend_pid() AND relation IN ('lf'::regclass, 'lf_pkey'::regclass) ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("relation", REGCLASS), Column("mode", TEXT), Column("granted", BOOL), Column("db", BOOL)],
                        rows: &[
                            &[T("lf"), T("RowExclusiveLock"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE lf IN SHARE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass, mode, granted FROM pg_locks WHERE pid = pg_backend_pid() AND relation = 'lf'::regclass ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("relation", REGCLASS), Column("mode", TEXT), Column("granted", BOOL)],
                        rows: &[
                            &[T("lf"), T("RowExclusiveLock"), T("t")],
                            &[T("lf"), T("ShareLock"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_xact_lock(5);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_xact_lock_shared(1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, classid, objid, objsubid, mode, granted, fastpath FROM pg_locks WHERE locktype = 'advisory' ORDER BY objsubid;",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("classid", OID), Column("objid", OID), Column("objsubid", INT2), Column("mode", TEXT), Column("granted", BOOL), Column("fastpath", BOOL)],
                        rows: &[
                            &[T("advisory"), T("0"), T("5"), T("1"), T("ExclusiveLock"), T("t"), T("f")],
                            &[T("advisory"), T("1"), T("2"), T("2"), T("ShareLock"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock(-1);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock(7, -3);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock_shared(8);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock_shared(8);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, classid, objid, objsubid, mode, granted, virtualtransaction LIKE '%/0' AS idle FROM pg_locks WHERE locktype = 'advisory' ORDER BY classid, objid;",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("classid", OID), Column("objid", OID), Column("objsubid", INT2), Column("mode", TEXT), Column("granted", BOOL), Column("idle", BOOL)],
                        rows: &[
                            &[T("advisory"), T("0"), T("8"), T("1"), T("ShareLock"), T("t"), T("f")],
                            &[T("advisory"), T("7"), T("4294967293"), T("2"), T("ExclusiveLock"), T("t"), T("f")],
                            &[T("advisory"), T("4294967295"), T("4294967295"), T("1"), T("ExclusiveLock"), T("t"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock_shared(8);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock_shared", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(8);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "you don't own a lock of type ExclusiveLock", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock_shared(8);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock_shared", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock_shared(8);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock_shared", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "you don't own a lock of type ShareLock", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(7, -3);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock(7, -3);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "you don't own a lock of type ExclusiveLock", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock_all();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock_all", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_lock(9);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD ALL;",
                    expected: Expected::Tag("DISCARD ALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_xact_lock(10);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_xact_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_xact_lock(11);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_xact_lock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE SAVEPOINT s;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_blocking_pids(pg_backend_pid());",
                    expected: Expected::Rows {
                        columns: &[Column("pg_blocking_pids", INT4_ARRAY)],
                        rows: &[
                            &[T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_safe_snapshot_blocking_pids(pg_backend_pid());",
                    expected: Expected::Rows {
                        columns: &[Column("pg_safe_snapshot_blocking_pids", INT4_ARRAY)],
                        rows: &[
                            &[T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_isolation_test_session_is_blocked(pg_backend_pid(), '{}');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_isolation_test_session_is_blocked", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_isolation_test_session_is_blocked(pg_backend_pid(), ARRAY[1, NULL]);",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "array must not contain nulls", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_blocking_pids(0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_blocking_pids", INT4_ARRAY)],
                        rows: &[
                            &[T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_try_advisory_lock(12), pg_try_advisory_lock_shared(12), pg_try_advisory_lock(12, 1), pg_try_advisory_lock_shared(13, 1);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_try_advisory_lock", BOOL), Column("pg_try_advisory_lock_shared", BOOL), Column("pg_try_advisory_lock", BOOL), Column("pg_try_advisory_lock_shared", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock_all();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock_all", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_locks and pg_blocking_pids show a session waiting for an advisory lock",
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_lock(1)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_lock(1)",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT locktype, objid, mode, granted FROM pg_locks WHERE locktype = 'advisory' ORDER BY granted DESC",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("objid", OID), Column("mode", TEXT), Column("granted", BOOL)],
                        rows: &[
                            &[T("advisory"), T("1"), T("ExclusiveLock"), T("t")],
                            &[T("advisory"), T("1"), T("ExclusiveLock"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pid = pg_backend_pid() AS own FROM pg_locks WHERE locktype = 'advisory' ORDER BY granted DESC",
                    expected: Expected::Rows {
                        columns: &[Column("own", BOOL)],
                        rows: &[
                            &[T("t")],
                            &[T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_blocking_pids(pid) = ARRAY[pg_backend_pid()] AS blocked FROM pg_locks WHERE NOT granted",
                    expected: Expected::Rows {
                        columns: &[Column("blocked", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_isolation_test_session_is_blocked(pid, ARRAY[pg_backend_pid()]) AS blocked FROM pg_locks WHERE NOT granted",
                    expected: Expected::Rows {
                        columns: &[Column("blocked", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_isolation_test_session_is_blocked(pid, ARRAY[0]) AS blocked FROM pg_locks WHERE NOT granted",
                    expected: Expected::Rows {
                        columns: &[Column("blocked", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT cardinality(pg_blocking_pids(pg_backend_pid())) AS count",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_unlock(1)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(1)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pg_locks WHERE locktype = 'advisory'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a session waiting on a unique key shows a wait for the transaction that holds the key",
            set_up_script: &[
                "CREATE TABLE lw (a INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO lw VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO lw VALUES (1)",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "lw_pkey""#, ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT locktype, mode, granted FROM pg_locks WHERE NOT granted",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("mode", TEXT), Column("granted", BOOL)],
                        rows: &[
                            &[T("transactionid"), T("ShareLock"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT w.transactionid = h.transactionid AS same FROM pg_locks w JOIN pg_locks h ON h.locktype = 'transactionid' AND h.granted AND h.pid = pg_backend_pid() WHERE NOT w.granted",
                    expected: Expected::Rows {
                        columns: &[Column("same", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_blocking_pids(pid) = ARRAY[pg_backend_pid()] AS blocked FROM pg_locks WHERE NOT granted",
                    expected: Expected::Rows {
                        columns: &[Column("blocked", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM lw",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_locks shows a session waiting for a table that LOCK TABLE holds",
            set_up_script: &[
                "CREATE TABLE lt2 (a INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ LOCK TABLE lt2 IN ACCESS EXCLUSIVE MODE",
                    expected: Expected::Tag("LOCK TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM lt2",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT relation::regclass AS relation, mode, granted FROM pg_locks WHERE relation = 'lt2'::regclass ORDER BY granted DESC",
                    expected: Expected::Rows {
                        columns: &[Column("relation", REGCLASS), Column("mode", TEXT), Column("granted", BOOL)],
                        rows: &[
                            &[T("lt2"), T("AccessExclusiveLock"), T("t")],
                            &[T("lt2"), T("AccessShareLock"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_blocking_pids(pid) = ARRAY[pg_backend_pid()] AS blocked FROM pg_locks WHERE NOT granted",
                    expected: Expected::Rows {
                        columns: &[Column("blocked", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM lt2",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_blocking_pids counts a session waiting ahead for a conflicting lock",
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_lock_shared(2)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock_shared", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_lock(2)",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT pg_advisory_lock_shared(2)",
                    flow: Flow::Exec,
                    client: "C",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT mode, granted, cardinality(pg_blocking_pids(pid)) AS count FROM pg_locks WHERE locktype = 'advisory' ORDER BY granted DESC, mode",
                    expected: Expected::Rows {
                        columns: &[Column("mode", TEXT), Column("granted", BOOL), Column("count", INT4)],
                        rows: &[
                            &[T("ShareLock"), T("t"), T("0")],
                            &[T("ExclusiveLock"), T("f"), T("1")],
                            &[T("ShareLock"), T("f"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_blocking_pids(w.pid) = ARRAY[e.pid] AS blocked FROM pg_locks w, pg_locks e WHERE w.locktype = 'advisory' AND NOT w.granted AND w.mode = 'ShareLock' AND e.locktype = 'advisory' AND e.mode = 'ExclusiveLock'",
                    expected: Expected::Rows {
                        columns: &[Column("blocked", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_blocking_pids(pid) = ARRAY[pg_backend_pid()] AS blocked FROM pg_locks WHERE locktype = 'advisory' AND mode = 'ExclusiveLock'",
                    expected: Expected::Rows {
                        columns: &[Column("blocked", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_unlock_shared(2)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock_shared", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock(2)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT pg_advisory_unlock_shared(2)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock_shared", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_signal_backends() {
    run_scripts(&[
        ScriptTest {
            name: "pg_cancel_backend and pg_terminate_backend signal sessions",
            set_up_script: &[
                "CREATE TABLE sb (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_cancel_backend(0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_cancel_backend", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "PID 0 is not a PostgreSQL backend process", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_terminate_backend(0, 4294967296);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_terminate_backend", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "PID 0 is not a PostgreSQL backend process", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_terminate_backend(0, 2147483648);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: r#""timeout" must not be negative"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_cancel_backend(pg_backend_pid());",
                    expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to user request", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 AS after_cancel;",
                    expected: Expected::Rows {
                        columns: &[Column("after_cancel", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE sb IN ACCESS SHARE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_sleep(60);",
                    expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to user request", ..E }),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_cancel_backend(pid) FROM pg_locks WHERE locktype = 'relation' AND relation = 'sb'::regclass AND pid <> pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_cancel_backend", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "LOCK TABLE sb IN ACCESS SHARE MODE;",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_sleep(60);",
                    expected: Expected::Error(Diagnostic { severity: "FATAL", code: "57P01", message: "terminating connection due to administrator command", ..E }),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_terminate_backend(pid, 10000) FROM pg_locks WHERE locktype = 'relation' AND relation = 'sb'::regclass AND pid <> pg_backend_pid();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_terminate_backend", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE locktype = 'relation' AND relation = 'sb'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_terminate_backend(pg_backend_pid());",
                    expected: Expected::Error(Diagnostic { severity: "FATAL", code: "57P01", message: "terminating connection due to administrator command", ..E }),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 2 AS after_terminate;",
                    expected: Expected::Rows {
                        columns: &[Column("after_terminate", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_locks_of_index_functions() {
    run_scripts(&[
        ScriptTest {
            name: "locks taken by the functions of index expressions are the caller's",
            set_up_script: &[
                "CREATE FUNCTION lf(i INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN PERFORM pg_advisory_lock(42); RETURN i; END $$;",
                "CREATE TABLE tl (a INT);",
                "CREATE INDEX ON tl (lf(a));",
                "CREATE FUNCTION xf(i INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN PERFORM pg_advisory_xact_lock(i); RETURN i; END $$;",
                "CREATE TABLE tx (a INT);",
                "CREATE INDEX ON tx (xf(a));",
                "CREATE FUNCTION ef(i INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN IF i = 3 THEN RAISE EXCEPTION 'bad %', i; END IF; RETURN i; END $$;",
                "CREATE TABLE te (a INT);",
                "CREATE INDEX ON te (ef(a));",
                "CREATE FUNCTION caller() RETURNS VOID LANGUAGE plpgsql AS $$ BEGIN INSERT INTO te VALUES (3); END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tl VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objid, mode, granted, pid = pg_backend_pid() AS mine FROM pg_locks WHERE locktype = 'advisory' ORDER BY objid;",
                    expected: Expected::Rows {
                        columns: &[Column("objid", OID), Column("mode", TEXT), Column("granted", BOOL), Column("mine", BOOL)],
                        rows: &[
                            &[T("42"), T("ExclusiveLock"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objid, mode, granted, pid = pg_backend_pid() AS mine FROM pg_locks WHERE locktype = 'advisory' ORDER BY objid;",
                    expected: Expected::Rows {
                        columns: &[Column("objid", OID), Column("mode", TEXT), Column("granted", BOOL), Column("mine", BOOL)],
                        rows: &[
                            &[T("42"), T("ExclusiveLock"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_advisory_unlock_all();",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tx VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tx VALUES (8);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objid, mode, granted, pid = pg_backend_pid() AS mine FROM pg_locks WHERE locktype = 'advisory' ORDER BY objid;",
                    expected: Expected::Rows {
                        columns: &[Column("objid", OID), Column("mode", TEXT), Column("granted", BOOL), Column("mine", BOOL)],
                        rows: &[
                            &[T("7"), T("ExclusiveLock"), T("t"), T("t")],
                            &[T("8"), T("ExclusiveLock"), T("t"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objid, mode, granted, pid = pg_backend_pid() AS mine FROM pg_locks WHERE locktype = 'advisory' ORDER BY objid;",
                    expected: Expected::Rows {
                        columns: &[Column("objid", OID), Column("mode", TEXT), Column("granted", BOOL), Column("mine", BOOL)],
                        rows: &[
                            &[T("7"), T("ExclusiveLock"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT caller();",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "bad 3", context: Some(r#"PL/pgSQL function ef(integer) line 1 at RAISE
SQL statement "INSERT INTO te VALUES (3)"
PL/pgSQL function caller() line 1 at SQL statement"#), ..E }),
                    skip: Some("index functions compile under the fixed settings of their own session, whose search path names them schema-qualified (Appendix C)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_a_transaction_shows_one_lock_on_its_own_id() {
    run_scripts(&[
        ScriptTest {
            name: "pg_locks shows one transactionid lock of a transaction whose subtransactions wrote",
            set_up_script: &[
                "CREATE TABLE pl_t (a int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pl_t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s1;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pl_t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s2;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pl_t VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE transactionid = pg_current_xact_id()::xid;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE pl_tmp (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE transactionid = pg_current_xact_id()::xid;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_waits_for_a_statistics_change_end_on_cancel_timeout_and_terminate() {
    run_scripts(&[ScriptTest {
        name: "a statistics change waiting for another transaction ends on a cancel, a timeout, and a terminate",
        set_up_script: &["CREATE TABLE e (a int);"],
        assertions: &[
            ScriptTestAssertion {
                query: "BEGIN;",
                expected: Expected::Tag("BEGIN"),
                client: "C",
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'e', 'reltuples', 5::real);",
                expected: Expected::Rows {
                    columns: &[Column("pg_restore_relation_stats", BOOL)],
                    rows: &[
                        &[T("t")],
                    ],
                    tag: "SELECT 1",
                },
                client: "C",
                ..A
            },
            ScriptTestAssertion {
                query: "UPDATE pg_class SET reltuples = 7 WHERE oid = 'e'::regclass;",
                expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to user request", ..E }),
                client: "B",
                expected_blocking: true,
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT wait_event_type, wait_event FROM pg_stat_activity WHERE query = 'UPDATE pg_class SET reltuples = 7 WHERE oid = ''e''::regclass;';",
                expected: Expected::Rows {
                    columns: &[Column("wait_event_type", TEXT), Column("wait_event", TEXT)],
                    rows: &[
                        &[T("Lock"), T("transactionid")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query = 'UPDATE pg_class SET reltuples = 7 WHERE oid = ''e''::regclass;';",
                expected: Expected::Rows {
                    columns: &[Column("pg_cancel_backend", BOOL)],
                    rows: &[
                        &[T("t")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SET statement_timeout = '500ms';",
                expected: Expected::Tag("SET"),
                client: "B",
                ..A
            },
            ScriptTestAssertion {
                query: "UPDATE pg_class SET reltuples = 7 WHERE oid = 'e'::regclass;",
                expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to statement timeout", ..E }),
                client: "B",
                ..A
            },
            ScriptTestAssertion {
                query: "SET statement_timeout = 0;",
                expected: Expected::Tag("SET"),
                client: "B",
                ..A
            },
            ScriptTestAssertion {
                query: "UPDATE pg_class SET reltuples = 7 WHERE oid = 'e'::regclass;",
                expected: Expected::Error(Diagnostic { severity: "FATAL", code: "57P01", message: "terminating connection due to administrator command", ..E }),
                client: "B",
                expected_blocking: true,
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query = 'UPDATE pg_class SET reltuples = 7 WHERE oid = ''e''::regclass;';",
                expected: Expected::Rows {
                    columns: &[Column("pg_terminate_backend", BOOL)],
                    rows: &[
                        &[T("t")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ROLLBACK;",
                expected: Expected::Tag("ROLLBACK"),
                client: "C",
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_statements_lock_the_indexes_they_plan_and_write() {
    run_scripts(&[
        ScriptTest {
            name: "a statement holds a lock on each index of a table it plans, a write's until the transaction ends and a generic plan's executor only while it writes",
            set_up_script: &[
                "CREATE TABLE il_t (id INTEGER PRIMARY KEY, k INTEGER);",
                "CREATE INDEX il_t_k ON il_t (k);",
                "INSERT INTO il_t VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE il_t SET k = 2 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'il\\_%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("il_t"), T("RowExclusiveLock")],
                            &[T("il_t_k"), T("RowExclusiveLock")],
                            &[T("il_t_pkey"), T("RowExclusiveLock")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM il_t WHERE k = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'il\\_%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("il_t"), T("AccessShareLock")],
                            &[T("il_t_k"), T("AccessShareLock")],
                            &[T("il_t_pkey"), T("AccessShareLock")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plan_cache_mode = force_generic_plan;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE il_u AS UPDATE il_t SET k = k + 1 WHERE id = $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE il_s AS SELECT * FROM il_t WHERE id = $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE il_u(1);",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE il_s(1);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE il_u(1);",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'il\\_%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("il_t"), T("RowExclusiveLock")],
                            &[T("il_t_pkey"), T("RowExclusiveLock")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE il_s(1);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'il\\_%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("il_t"), T("AccessShareLock")],
                            &[T("il_t_pkey"), T("AccessShareLock")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM il_t WHERE id = 5;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'il\\_%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("il_t"), T("RowExclusiveLock")],
                            &[T("il_t_k"), T("RowExclusiveLock")],
                            &[T("il_t_pkey"), T("RowExclusiveLock")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row routed to a partition holds the partition's lock and its indexes' only while the statement writes",
            set_up_script: &[
                "CREATE TABLE il_p (id INTEGER PRIMARY KEY, k INTEGER) PARTITION BY RANGE (id);",
                "CREATE TABLE il_p1 PARTITION OF il_p FOR VALUES FROM (0) TO (100);",
                "CREATE INDEX il_p_k ON il_p (k);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO il_p VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'il\\_%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("il_p"), T("RowExclusiveLock")],
                            &[T("il_p1"), T("RowExclusiveLock")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE il_p SET k = 5 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'il\\_%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("il_p"), T("AccessShareLock")],
                            &[T("il_p"), T("RowExclusiveLock")],
                            &[T("il_p1"), T("RowExclusiveLock")],
                            &[T("il_p1_k_idx"), T("RowExclusiveLock")],
                            &[T("il_p1_pkey"), T("RowExclusiveLock")],
                            &[T("il_p_k"), T("RowExclusiveLock")],
                            &[T("il_p_pkey"), T("RowExclusiveLock")],
                        ],
                        tag: "SELECT 7",
                    },
                    skip: Some("7409: a partition's constraint check takes no AccessShareLock on the partition's parent, which Postgres' generate_partition_qual takes"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_waits_for_rows_name_what_they_waited_for() {
    run_scripts(&[
        ScriptTest {
            name: "a wait for another transaction's row that times out names what the statement did to which row of which relation",
            set_up_script: &[
                "CREATE TABLE lw (id INTEGER PRIMARY KEY, k INTEGER);",
                "CREATE INDEX lw_k ON lw (k);",
                "INSERT INTO lw VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE lw SET k = 3 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lw VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '100ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM lw WHERE id = 1 FOR SHARE;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while locking tuple (0,1) in relation "lw""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lw VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while inserting index tuple (0,4) in relation "lw_pkey""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE lw SET k = 9 WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while updating tuple (0,1) in relation "lw""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM lw WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while deleting tuple (0,1) in relation "lw""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM lw WHERE id = 1 FOR UPDATE NOWAIT;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: r#"could not obtain lock on row in relation "lw""#, context: Some(""), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lw VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while inserting index tuple (0,5) in relation "lw_pkey""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a wait on a unique secondary index's entry names the index and the new row",
            set_up_script: &[
                "CREATE TABLE lu (id INTEGER PRIMARY KEY, v INTEGER UNIQUE);",
                "INSERT INTO lu VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lu VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '100ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lu VALUES (3, 2);",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while inserting index tuple (0,3) in relation "lu_v_key""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an ON CONFLICT insert waiting on another transaction's row names the row and the table",
            set_up_script: &[
                "CREATE TABLE lc (id INTEGER PRIMARY KEY, v INTEGER);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lc VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '100ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lc VALUES (2, 9) ON CONFLICT DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while inserting index tuple (0,1) in relation "lc""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lc VALUES (2, 9) ON CONFLICT (id) DO UPDATE SET v = 0;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while inserting index tuple (0,1) in relation "lc""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an insert waiting on a row an exclusion constraint compares it with names the row",
            set_up_script: &[
                "CREATE TABLE lx (id INTEGER PRIMARY KEY, r INTEGER, EXCLUDE USING btree (r WITH =));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lx VALUES (1, 7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '100ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lx VALUES (2, 7);",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while checking exclusion constraint on tuple (0,1) in relation "lx""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a lock that follows an update to a newer version another transaction locked names the version",
            set_up_script: &[
                "CREATE TABLE lt (id INTEGER PRIMARY KEY, k INTEGER);",
                "INSERT INTO lt VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM lt;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE lt SET k = 5 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM lt WHERE id = 1 FOR UPDATE;",
                    expected: Expected::Rows { columns: &[Column("id", INT4), Column("k", INT4)], rows: &[&[T("1"), T("5")]], tag: "SELECT 1" },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '100ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM lt WHERE id = 1 FOR KEY SHARE;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while locking updated version (0,2) of tuple in relation "lt""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a wait for another transaction adding an identical row of a table without a primary key names the index entry or the row",
            set_up_script: &[
                "CREATE TABLE su (a INTEGER);",
                "CREATE UNIQUE INDEX su_a ON su (a);",
                "INSERT INTO su VALUES (0);",
                "CREATE TABLE se (a INTEGER, EXCLUDE USING btree (a WITH =));",
                "INSERT INTO se VALUES (0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO su VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO se VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '100ms';",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO su VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while inserting index tuple (0,3) in relation "su_a""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO se VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", context: Some(r#"while checking exclusion constraint on tuple (0,2) in relation "se""#), ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_a_lock_taken_again_is_given_back_once_for_each_take() {
    run_scripts(&[
        ScriptTest {
            name: "a trigger's statement that locks an index the outer statement holds gives back only its own lock",
            set_up_script: &[
                "CREATE TABLE rt (id INTEGER PRIMARY KEY, k INTEGER);",
                "CREATE INDEX rt_k ON rt (k);",
                "INSERT INTO rt VALUES (1, 1);",
                "CREATE FUNCTION rt_touch() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN EXECUTE 'DELETE FROM rt WHERE id = -1'; RETURN NEW; END $$;",
                "CREATE TRIGGER rt_after AFTER UPDATE ON rt FOR EACH ROW EXECUTE FUNCTION rt_touch();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET plan_cache_mode = force_generic_plan;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE ru AS UPDATE rt SET k = k + 1 WHERE id = $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE ru(1);",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE ru(1);",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'rt%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("rt"), T("RowExclusiveLock")],
                            &[T("rt_k"), T("RowExclusiveLock")],
                            &[T("rt_pkey"), T("RowExclusiveLock")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a trigger's statement that opens a partition first leaves the partition's index locks to each row that writes it",
            set_up_script: &[
                "CREATE TABLE pt (id INTEGER, k INTEGER) PARTITION BY RANGE (id);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (100);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (100) TO (200);",
                "CREATE UNIQUE INDEX pt_id ON pt (id);",
                "CREATE TABLE seen (at INTEGER, rel TEXT, mode TEXT);",
                "CREATE FUNCTION pt_watch() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.id = 101 THEN EXECUTE 'INSERT INTO pt1 VALUES (50, 0)'; END IF; INSERT INTO seen SELECT NEW.id, relation::regclass::text, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'pt1%'; RETURN NEW; END $$;",
                "CREATE TRIGGER pt_before BEFORE INSERT ON pt FOR EACH ROW EXECUTE FUNCTION pt_watch();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (101, 0), (2, 0), (102, 0);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT at, rel, mode FROM seen ORDER BY at, rel COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("at", INT4), Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("2"), T("pt1"), T("RowExclusiveLock")],
                            &[T("2"), T("pt1_id_idx"), T("RowExclusiveLock")],
                            &[T("50"), T("pt1"), T("RowExclusiveLock")],
                            &[T("50"), T("pt1_id_idx"), T("RowExclusiveLock")],
                            &[T("101"), T("pt1"), T("RowExclusiveLock")],
                            &[T("102"), T("pt1"), T("RowExclusiveLock")],
                            &[T("102"), T("pt1_id_idx"), T("RowExclusiveLock")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass::text AS rel, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'pt1%' ORDER BY relation::regclass::text COLLATE \"C\", mode;",
                    expected: Expected::Rows {
                        columns: &[Column("rel", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("pt1"), T("RowExclusiveLock")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
