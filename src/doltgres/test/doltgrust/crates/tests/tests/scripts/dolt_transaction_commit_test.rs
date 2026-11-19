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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_transaction_commit_test.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_transaction_commit_one_client() {
    run_scripts(&[
        ScriptTest {
            name: "dolt commit on transaction commit one client",
            intent: "With dolt_transaction_commit on, a session's transaction commit makes a Dolt commit of the working set, with dolt_transaction_commit_message as its message once set, and another session's commits make none.",
            set_up_script: &[
                "CREATE TABLE x (y BIGINT PRIMARY KEY, z BIGINT);",
                "INSERT INTO x VALUES (1,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.initial_head', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.initial_head', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'Commit Message 42';",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE newTable (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres has no autocommit setting: a statement outside a transaction block commits on its own, so COMMIT warns that no transaction is in progress, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log ORDER BY date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit Message 42")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Commit Message 42")],
                            &[T("Transaction commit")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    dolt: Some(Dolt::Setup("Dolt's harness starts from its checkpoint commit; ours shows the two transactions' commits and the initial one")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_transaction_commit_two_clients() {
    run_scripts(&[
        ScriptTest {
            name: "dolt commit on transaction commit two clients",
            intent: "Two sessions with dolt_transaction_commit on each make a Dolt commit with their own message when their transactions commit, in commit order.",
            set_up_script: &[
                "CREATE TABLE x (y BIGINT PRIMARY KEY, z BIGINT);",
                "INSERT INTO x VALUES (1,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (4, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.initial_head', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.initial_head', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'ClientB Commit';",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'ClientA Commit';",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.initial_head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x AS OF 'HEAD' ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "c",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff('HEAD', 'WORKING', 'x');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("ClientA Commit")],
                            &[T("ClientB Commit")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    dolt: Some(Dolt::Setup("Dolt's harness starts from its checkpoint commit; ours shows the two clients' commits and the initial one")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_transaction_commit_autocommit() {
    run_scripts(&[
        ScriptTest {
            name: "dolt commit with autocommit",
            intent: "With dolt_transaction_commit on, each statement that commits on its own makes a Dolt commit, and a statement that changes nothing makes none.",
            set_up_script: &[
                "CREATE TABLE x (y BIGINT PRIMARY KEY, z BIGINT);",
                "INSERT INTO x VALUES (1,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'ClientB Commit';",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT8), Column("z", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("ClientB Commit")],
                            &[T("Transaction commit")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    dolt: Some(Dolt::Setup("Dolt's harness starts from its checkpoint commit; ours shows the two statements' commits and the initial one")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
