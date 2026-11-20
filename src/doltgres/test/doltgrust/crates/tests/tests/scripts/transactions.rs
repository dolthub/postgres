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
fn test_transactions() {
    run_scripts(&[
        ScriptTest {
            name: "the rows of a subtransaction and of its parent, Postgres' transactions.sql",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE savepoints (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO savepoints VALUES (6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT one;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO savepoints VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE SAVEPOINT one;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO savepoints VALUES (8);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.xmin = b.xmin FROM savepoints a, savepoints b WHERE a.a=6 AND b.a=8;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.xmin = b.xmin FROM savepoints a, savepoints b WHERE a.a=6 AND b.a=7;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Subtransactions get no transaction ids of their own: rows a subtransaction writes or locks show the top transaction's id in xmin and xmax, where Postgres shows the subtransaction's xid (Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO savepoints VALUES (9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT one;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO savepoints VALUES (10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT one;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO savepoints VALUES (11);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM savepoints WHERE a IN (9, 10, 11) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("9")],
                            &[T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.xmin = b.xmin FROM savepoints a, savepoints b WHERE a.a=9 AND b.a=11;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Subtransactions get no transaction ids of their own: rows a subtransaction writes or locks show the top transaction's id in xmin and xmax, where Postgres shows the subtransaction's xid (Appendix C)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_transaction_read_only_set_in_a_savepoint() {
    run_scripts(&[
        ScriptTest {
            name: "SET TRANSACTION READ ONLY in a savepoint lasts until the savepoint is released or rolled back to, as Postgres' subtransactions restore the mode they started with",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT x;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TRANSACTION READ ONLY;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT x;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_read_only;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_read_only", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT y;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TRANSACTION READ ONLY;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE SAVEPOINT y;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_read_only;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_read_only", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT z;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TRANSACTION READ ONLY;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1/0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT z;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_read_only;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_read_only", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TRANSACTION READ ONLY;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT w;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE SAVEPOINT w;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_read_only;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_read_only", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
