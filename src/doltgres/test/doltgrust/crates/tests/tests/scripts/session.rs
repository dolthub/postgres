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
fn test_begin_isolation_level() {
    run_scripts(&[
        ScriptTest {
            name: "BEGIN with any isolation level clause is accepted as a no-op",
            set_up_script: &[
                "CREATE TABLE test (a INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN TRANSACTION ISOLATION LEVEL READ COMMITTED",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL READ UNCOMMITTED",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ, READ WRITE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A duplicate BEGIN does not change the active transaction's characteristics",
            set_up_script: &[
                "CREATE TABLE test (a INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL SERIALIZABLE, READ WRITE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY",
                    expected: Expected::Tag("BEGIN"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25001", message: "there is already a transaction in progress", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1)",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ROLLBACK clears the in-transaction flag so a following BEGIN is honored",
            set_up_script: &[
                "CREATE TABLE test_rollback (a INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN READ WRITE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN READ ONLY",
                    expected: Expected::Tag("BEGIN"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25001", message: "there is already a transaction in progress", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN READ ONLY",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test_rollback VALUES (1)",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_discard() {
    run_scripts(&[
        ScriptTest {
            name: "Test discard",
            set_up_script: &[
                "CREATE temporary TABLE test (a INT)",
                "insert into test values (1)",
                "SET search_path = pg_catalog",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SHOW search_path",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T("pg_catalog")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD ALL",
                    expected: Expected::Tag("DISCARD ALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "test" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW search_path",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T(r#""$user", public"#)],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test discard errors",
            set_up_script: &[
                "CREATE temporary TABLE test (a INT)",
                "insert into test values (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DISCARD SEQUENCES",
                    expected: Expected::Tag("DISCARD SEQUENCES"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test discard in transaction",
            set_up_script: &[
                "CREATE temporary TABLE test (a INT)",
                "insert into test values (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD ALL",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "DISCARD ALL cannot run inside a transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_rollback() {
    run_scripts(&[
        ScriptTest {
            name: "Test rollback transaction",
            set_up_script: &[
                "BEGIN",
                "CREATE temporary TABLE test (a INT)",
                "insert into test values (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "test" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "create temp table test (b int)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_session_state_after_query_error() {
    run_scripts(&[
        ScriptTest {
            name: "Test failed query does not pin the session to a stale root",
            set_up_script: &[
                "CREATE TABLE test (a INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM doesnotexist",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "postgres",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test failed root object lookup does not pin the session to a stale root",
            set_up_script: &[
                "CREATE TABLE test (a INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT doesnotexist()",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function doesnotexist() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE seq",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    username: "postgres",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('seq')",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test failed query inside a transaction aborts the transaction",
            set_up_script: &[
                "CREATE TABLE test (a INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION",
                    expected: Expected::Tag("START TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM doesnotexist",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "postgres",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("ROLLBACK"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
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
fn test_read_only_transaction_rules() {
    run_scripts(&[
        ScriptTest {
            name: "Read-only transactions",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (a INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL SERIALIZABLE, READ WRITE;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY;",
                    expected: Expected::Tag("BEGIN"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25001", message: "there is already a transaction in progress", ..E }],
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
                    query: "SHOW transaction_isolation;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_isolation", TEXT)],
                        rows: &[
                            &[T("repeatable read")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
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
                    query: "BEGIN READ ONLY;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (a int);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute CREATE TABLE in a read-only transaction", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_transaction_read_only = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
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
                    query: "DELETE FROM test;",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute DELETE in a read-only transaction", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN READ WRITE;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_transaction_read_only = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION READ ONLY;",
                    expected: Expected::Tag("START TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET a = 4;",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute UPDATE in a read-only transaction", ..E }),
                    flow: Flow::Query,
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
fn test_discard_and_hidden_setting_rules() {
    run_scripts(&[
        ScriptTest {
            name: "DISCARD forms",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE ds;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('ds');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD SEQUENCES;",
                    expected: Expected::Tag("DISCARD SEQUENCES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT currval('ds');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"currval of sequence "ds" is not yet defined in this session"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lastval();",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "lastval is not yet defined in this session", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD PLANS;",
                    expected: Expected::Tag("DISCARD PLANS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD TEMP;",
                    expected: Expected::Tag("DISCARD TEMP"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD TEMPORARY;",
                    expected: Expected::Tag("DISCARD TEMP"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('ds');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("2")],
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
                    query: "SELECT lastval();",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "lastval is not yet defined in this session", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD PLANS;",
                    expected: Expected::Tag("DISCARD PLANS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD SEQUENCES;",
                    expected: Expected::Tag("DISCARD SEQUENCES"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD ALL;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "DISCARD ALL cannot run inside a transaction block", ..E }),
                    flow: Flow::Query,
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
            name: "Hidden default_with_oids",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET default_with_oids = false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW default_with_oids;",
                    expected: Expected::Rows {
                        columns: &[Column("default_with_oids", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_with_oids = true;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "tables declared WITH OIDS are not supported", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, setting, category, short_desc, vartype, context FROM pg_settings WHERE name = 'default_with_oids';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT), Column("category", TEXT), Column("short_desc", TEXT), Column("vartype", TEXT), Column("context", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET default_with_oids;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_sql_cursors() {
    run_scripts(&[
        ScriptTest {
            name: "SQL cursors fetch, move, and close",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE cur_t (id INT PRIMARY KEY, v TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO cur_t SELECT i, 'v' || i FROM generate_series(1, 10) i;",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT id FROM cur_t ORDER BY id;",
                    expected: Expected::Error(Diagnostic { code: "25P01", message: "DECLARE CURSOR can only be used in transaction blocks", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT id, v FROM cur_t ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("v1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("v2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("3"), T("v3")],
                            &[T("4"), T("v4")],
                            &[T("5"), T("v5")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FORWARD 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("6"), T("v6")],
                            &[T("7"), T("v7")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("6"), T("v6")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("v5")],
                            &[T("4"), T("v4")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 9 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("9"), T("v9")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE -2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("7"), T("v7")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 0 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("7"), T("v7")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("10"), T("v10")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FIRST FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("v1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE -3 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("8"), T("v8")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 20 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("10"), T("v10")],
                            &[T("9"), T("v9")],
                            &[T("8"), T("v8")],
                            &[T("7"), T("v7")],
                            &[T("6"), T("v6")],
                            &[T("5"), T("v5")],
                            &[T("4"), T("v4")],
                            &[T("3"), T("v3")],
                            &[T("2"), T("v2")],
                            &[T("1"), T("v1")],
                        ],
                        tag: "FETCH 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE 4 IN c;",
                    expected: Expected::Tag("MOVE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("v5")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE FORWARD ALL IN c;",
                    expected: Expected::Tag("MOVE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42P03", message: r#"cursor "c" already exists"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH nosuch;",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    flow: Flow::Query,
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
                    query: "DECLARE d NO SCROLL CURSOR FOR SELECT id FROM cur_t ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM d;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
                    flow: Flow::Query,
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
                    query: "DECLARE e CURSOR WITH HOLD FOR SELECT id FROM cur_t WHERE id > 7 ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE f CURSOR FOR SELECT 1 AS one;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM e;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("8")],
                            &[T("9")],
                            &[T("10")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH f;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "f" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE e;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE e;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "e" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE g CURSOR FOR SELECT 1 AS one;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h CURSOR FOR SELECT 2 AS two;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ALL;",
                    expected: Expected::Tag("CLOSE CURSOR ALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH g;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "g" does not exist"#, ..E }),
                    flow: Flow::Query,
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
fn test_transaction_modes() {
    run_scripts(&[
        ScriptTest {
            name: "SET TRANSACTION and SET SESSION CHARACTERISTICS follow Postgres' rules",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;",
                    expected: Expected::Tag("SET"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET TRANSACTION can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_isolation;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_isolation", TEXT)],
                        rows: &[
                            &[T("read committed")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "SET TRANSACTION ISOLATION LEVEL must be called before any query", ..E }),
                    flow: Flow::Query,
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
                    query: "SAVEPOINT a;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction", ..E }),
                    flow: Flow::Query,
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
                    query: "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY, DEFERRABLE;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_isolation;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_isolation", TEXT)],
                        rows: &[
                            &[T("repeatable read")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_deferrable;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_deferrable", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN READ ONLY;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TRANSACTION READ WRITE;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "transaction read-write mode must be set before any query", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_isolation;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_isolation", TEXT)],
                        rows: &[
                            &[T("repeatable read")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW transaction_isolation;",
                    expected: Expected::Rows {
                        columns: &[Column("transaction_isolation", TEXT)],
                        rows: &[
                            &[T("repeatable read")],
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

#[test]
fn test_checkpoint() {
    run_scripts(&[
        ScriptTest {
            name: "CHECKPOINT runs inside and outside a transaction block, and only for superusers until roles exist",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CHECKPOINT",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE nc LOGIN",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE nc",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to execute CHECKPOINT command", detail: r#"Only roles with privileges of the "pg_checkpoint" role may execute this command."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_serializable_isolation() {
    run_scripts(&[
        ScriptTest {
            name: "serializable transactions fail on write skew, the doctors on call",
            set_up_script: &[
                "CREATE TABLE doctors (name TEXT PRIMARY KEY, on_call BOOL)",
                "INSERT INTO doctors VALUES ('alice', true), ('bob', true)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM doctors WHERE on_call",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM doctors WHERE on_call",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE doctors SET on_call = false WHERE name = 'alice'",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE doctors SET on_call = false WHERE name = 'bob'",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during commit attempt.", hint: "The transaction might succeed if retried.", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM doctors WHERE on_call",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a serializable read-only deferrable transaction waits for a safe snapshot",
            set_up_script: &[
                "CREATE TABLE doctors (name TEXT PRIMARY KEY, on_call BOOL)",
                "INSERT INTO doctors VALUES ('alice', true), ('bob', true)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM doctors WHERE on_call",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE doctors SET on_call = false WHERE name = 'alice'",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE, READ ONLY, DEFERRABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM doctors WHERE on_call",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM doctors WHERE on_call",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "serializable transactions fail on write skew through an index whose operator class orders keys by its comparison function",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 > $2 THEN -1 ELSE 1 END $$",
                "CREATE OPERATOR ~<~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4gt)",
                "CREATE OPERATOR ~<=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4ge)",
                "CREATE OPERATOR ~>=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4le)",
                "CREATE OPERATOR ~>~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4lt)",
                "CREATE OPERATOR CLASS rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 ~<~, OPERATOR 2 ~<=~, OPERATOR 3 =, OPERATOR 4 ~>=~, OPERATOR 5 ~>~, FUNCTION 1 rev_cmp(INT4, INT4)",
                "CREATE TABLE shifts (id INT PRIMARY KEY, a INT)",
                "INSERT INTO shifts VALUES (1, 2), (2, 8)",
                "CREATE INDEX shifts_a ON shifts (a rev_ops)",
                "SET enable_seqscan = off",
                "SET enable_bitmapscan = off",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_bitmapscan = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM shifts WHERE a ~<~ 5",
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
                    query: "/* client B */ SELECT count(*) FROM shifts WHERE a ~>~ 5",
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
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO shifts VALUES (3, 1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO shifts VALUES (4, 9)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during commit attempt.", hint: "The transaction might succeed if retried.", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM shifts ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("8")],
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "serializable DROP TABLE conflicts with transactions that read the table",
            set_up_script: &[
                "CREATE TABLE x (id INT PRIMARY KEY, v INT)",
                "INSERT INTO x VALUES (1, 1)",
                "CREATE TABLE u (id INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM x",
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
                    query: "/* client B */ SELECT count(*) FROM u",
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
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO u VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DROP TABLE x",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during write.", hint: "The transaction might succeed if retried.", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "serializable TRUNCATE conflicts with transactions that read the table",
            set_up_script: &[
                "CREATE TABLE x (id INT PRIMARY KEY, v INT)",
                "INSERT INTO x VALUES (1, 1)",
                "CREATE TABLE u (id INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM x",
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
                    query: "/* client B */ SELECT count(*) FROM u",
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
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO u VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ TRUNCATE x",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during write.", hint: "The transaction might succeed if retried.", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a table rewrite keeps the predicate locks of transactions that read the table",
            set_up_script: &[
                "CREATE TABLE x (id INT PRIMARY KEY, v INT)",
                "INSERT INTO x VALUES (1, 1), (2, 2)",
                "CREATE TABLE u (id INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT v FROM x WHERE id = 1",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM u",
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
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO u VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE x ALTER COLUMN id TYPE BIGINT",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE x SET v = 10 WHERE id = 1",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during write.", hint: "The transaction might succeed if retried.", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_locks shows the predicate locks of serializable transactions",
            set_up_script: &[
                "CREATE TABLE tidscan (id INT)",
                "INSERT INTO tidscan VALUES (1), (2), (3)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tidscan WHERE ctid = '(0,1)'",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, mode FROM pg_locks WHERE pid = pg_backend_pid() AND mode = 'SIReadLock'",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("mode", TEXT)],
                        rows: &[
                            &[T("tuple"), T("SIReadLock")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, relation::regclass, page, tuple, virtualtransaction = (SELECT virtualtransaction FROM pg_locks WHERE locktype = 'virtualxid' AND pid = pg_backend_pid()) AS mine, granted, fastpath FROM pg_locks WHERE mode = 'SIReadLock'",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("relation", REGCLASS), Column("page", INT4), Column("tuple", INT2), Column("mine", BOOL), Column("granted", BOOL), Column("fastpath", BOOL)],
                        rows: &[
                            &[T("tuple"), T("tidscan"), T("0"), T("1"), T("t"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM tidscan",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, relation::regclass, page, tuple FROM pg_locks WHERE mode = 'SIReadLock'",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("relation", REGCLASS), Column("page", INT4), Column("tuple", INT2)],
                        rows: &[
                            &[T("relation"), T("tidscan"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_locks WHERE mode = 'SIReadLock'",
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
            name: "a serializable transaction's third row lock on one page becomes a lock on the page",
            set_up_script: &[
                "CREATE TABLE p (id INT, v INT)",
                "INSERT INTO p SELECT i, i FROM generate_series(1, 10) i",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM p WHERE ctid = '(0,1)'",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM p WHERE ctid = '(0,2)'",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, page, tuple FROM pg_locks WHERE mode = 'SIReadLock' ORDER BY 1, 2, 3",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("page", INT4), Column("tuple", INT2)],
                        rows: &[
                            &[T("tuple"), T("0"), T("1")],
                            &[T("tuple"), T("0"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM p WHERE ctid = '(0,3)'",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, page, tuple FROM pg_locks WHERE mode = 'SIReadLock' ORDER BY 1, 2, 3",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("page", INT4), Column("tuple", INT2)],
                        rows: &[
                            &[T("page"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM p WHERE ctid = '(0,4)'",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT locktype, page, tuple FROM pg_locks WHERE mode = 'SIReadLock' ORDER BY 1, 2, 3",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("page", INT4), Column("tuple", INT2)],
                        rows: &[
                            &[T("page"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "serializable transactions fail on write skew through a GiST index whose inserts widen the unions of subtrees the other searches passed over",
            set_up_script: &[
                "CREATE TABLE pts (id INT PRIMARY KEY, p POINT)",
                "INSERT INTO pts SELECT i, point(i % 50, i / 50) FROM generate_series(0, 2499) i",
                "INSERT INTO pts SELECT 2500 + i, point(1000 + i % 50, 1000 + i / 50) FROM generate_series(0, 2499) i",
                "CREATE INDEX pts_p ON pts USING gist (p)",
                "CHECKPOINT",
                "SET enable_seqscan = off",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pts WHERE p <@ box '((0,0),(49,49))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pts WHERE p <@ box '((1000,1000),(1049,1049))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2500")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO pts VALUES (5001, point(5000,5000))",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO pts VALUES (5002, point(-5000,-5000))",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during commit attempt.", hint: "The transaction might succeed if retried.", ..E }),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "serializable transactions both commit when their inserts into a GiST index fit the unions of subtrees the other searches passed over",
            set_up_script: &[
                "CREATE TABLE pts (id INT PRIMARY KEY, p POINT)",
                "INSERT INTO pts SELECT i, point(i % 50, i / 50) FROM generate_series(0, 2499) i",
                "INSERT INTO pts SELECT 2500 + i, point(1000 + i % 50, 1000 + i / 50) FROM generate_series(0, 2499) i",
                "CREATE INDEX pts_p ON pts USING gist (p)",
                "CHECKPOINT",
                "SET enable_seqscan = off",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pts WHERE p <@ box '((0,0),(9,9))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pts WHERE p <@ box '((1000,1000),(1009,1009))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO pts VALUES (5001, point(1049,1049))",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO pts VALUES (5002, point(49,49))",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "serializable transactions both commit when they read and write disjoint ranges of an index whose operator class orders keys by its comparison function",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 > $2 THEN -1 ELSE 1 END $$",
                "CREATE OPERATOR ~<~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4gt)",
                "CREATE OPERATOR ~<=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4ge)",
                "CREATE OPERATOR ~>=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4le)",
                "CREATE OPERATOR ~>~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4lt)",
                "CREATE OPERATOR CLASS rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 ~<~, OPERATOR 2 ~<=~, OPERATOR 3 =, OPERATOR 4 ~>=~, OPERATOR 5 ~>~, FUNCTION 1 rev_cmp(INT4, INT4)",
                "CREATE TABLE shifts (id INT PRIMARY KEY, a INT)",
                "INSERT INTO shifts SELECT i, i FROM generate_series(1, 10000) i",
                "CREATE INDEX shifts_a ON shifts (a rev_ops)",
                "CHECKPOINT",
                "SET enable_seqscan = off",
                "SET enable_bitmapscan = off",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_bitmapscan = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM shifts WHERE a ~>~ 10",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM shifts WHERE a ~<~ 9990",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO shifts VALUES (10001, 5000)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO shifts VALUES (10002, 5001)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "serializable transactions fail on write skew through a GiST index whose entries are all in memory",
            set_up_script: &[
                "CREATE TABLE pts (id INT PRIMARY KEY, p POINT)",
                "CREATE INDEX pts_p ON pts USING gist (p)",
                "INSERT INTO pts SELECT i, point(i % 50, i / 50) FROM generate_series(0, 2499) i",
                "INSERT INTO pts SELECT 2500 + i, point(1000 + i % 50, 1000 + i / 50) FROM generate_series(0, 2499) i",
                "SET enable_seqscan = off",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pts WHERE p <@ box '((0,0),(49,49))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pts WHERE p <@ box '((1000,1000),(1049,1049))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2500")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO pts VALUES (5001, point(5000,5000))",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO pts VALUES (5002, point(-5000,-5000))",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during commit attempt.", hint: "The transaction might succeed if retried.", ..E }),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "serializable transactions both commit when they read and insert apart in a GiST index whose entries are all in memory",
            set_up_script: &[
                "CREATE TABLE pts (id INT PRIMARY KEY, p POINT)",
                "CREATE INDEX pts_p ON pts USING gist (p)",
                "INSERT INTO pts SELECT i, point(i % 50, i / 50) FROM generate_series(0, 2499) i",
                "INSERT INTO pts SELECT 2500 + i, point(1000 + i % 50, 1000 + i / 50) FROM generate_series(0, 2499) i",
                "SET enable_seqscan = off",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pts WHERE p <@ box '((0,0),(9,9))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pts WHERE p <@ box '((1000,1000),(1009,1009))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO pts VALUES (5001, point(1049,1049))",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO pts VALUES (5002, point(49,49))",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_serializable_primary_key_lookups() {
    run_scripts(&[
        ScriptTest {
            name: "a primary key lookup of a missing key conflicts with no change of the keys after it",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)",
                "INSERT INTO t VALUES (1, 'a'), (2, 'b')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM t WHERE id = 0",
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
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT v FROM t WHERE id = 2",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE t SET v = 'x' WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE t SET v = 'y' WHERE id = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM t ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a primary key lookup of a missing key conflicts with an insert of that key",
            set_up_script: &[
                "CREATE TABLE u (id INT PRIMARY KEY, v TEXT)",
                "INSERT INTO u VALUES (1, 'a'), (2, 'b')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM u WHERE id = 0",
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
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT v FROM u WHERE id = 2",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO u VALUES (0, 'z')",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE u SET v = 'y' WHERE id = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during commit attempt.", hint: "The transaction might succeed if retried.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a lookup by a primary key's leading column conflicts with an insert of that value past every existing key",
            set_up_script: &[
                "CREATE TABLE m (a INT, b INT, v TEXT, PRIMARY KEY (a, b))",
                "INSERT INTO m SELECT 1, g FROM generate_series(1, 500) g",
                "INSERT INTO m SELECT a, g FROM generate_series(2, 3) a, generate_series(1, 1000) g",
                "ANALYZE m",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SET enable_bitmapscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM m WHERE a = 1",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT v FROM m WHERE a = 3 AND b = 1000",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO m VALUES (1, 999)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE m SET v = 'y' WHERE a = 3 AND b = 1000",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during commit attempt.", hint: "The transaction might succeed if retried.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a lookup by a primary key's leading column conflicts with no insert of another value",
            set_up_script: &[
                "CREATE TABLE m (a INT, b INT, v TEXT, PRIMARY KEY (a, b))",
                "INSERT INTO m SELECT 1, g FROM generate_series(1, 500) g",
                "INSERT INTO m SELECT a, g FROM generate_series(2, 3) a, generate_series(1, 1000) g",
                "ANALYZE m",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SET enable_bitmapscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM m WHERE a = 1",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT v FROM m WHERE a = 3 AND b = 1000",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO m VALUES (3, 0)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE m SET v = 'z' WHERE a = 3 AND b = 1000",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_serializable_reads_of_one_key() {
    run_scripts(&[
        ScriptTest {
            name: "a lookup by primary key conflicts with no writer of a later key it doesn't read, as Postgres' multiple-row-versions spec finds",
            set_up_script: &[
                "CREATE TABLE t (id INT NOT NULL, txt TEXT) WITH (fillfactor = 50)",
                "INSERT INTO t (id) SELECT x FROM generate_series(1, 100000) x",
                "ALTER TABLE t ADD PRIMARY KEY (id)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ BEGIN ISOLATION LEVEL SERIALIZABLE",
                    expected: Expected::Tag("BEGIN"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT id FROM t WHERE id = 100000",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("100000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE t SET txt = 'b' WHERE id = 100000",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ UPDATE t SET txt = 'c' WHERE id = 100000",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT id FROM t WHERE id = 50000",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("50000")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ UPDATE t SET txt = 'd' WHERE id = 50000",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ SELECT id FROM t WHERE id = 1",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client D */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "D",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE t SET txt = 'a' WHERE id = 1",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during write.", hint: "The transaction might succeed if retried.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_changes_another_session_commits() {
    run_scripts(&[
        ScriptTest {
            name: "a table's new column another session committed is seen by the next statement",
            set_up_script: &["CREATE TABLE cc (a INT);", "INSERT INTO cc VALUES (1);"],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM cc;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE cc ADD COLUMN b INT DEFAULT 2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cc;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[&[T("1"), T("2")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE cc;",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cc;",
                    expected: Expected::Error(Diagnostic {
                        code: "42P01",
                        message: r#"relation "cc" does not exist"#,
                        position: 15,
                        ..E
                    }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "statistics another session's ANALYZE committed are seen by the next statement",
            set_up_script: &["CREATE TABLE cs (a INT);"],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 'cs';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[&[T("-1")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO cs SELECT generate_series(1, 5);",
                    expected: Expected::Tag("INSERT 0 5"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion { query: "ANALYZE cs;", expected: Expected::Tag("ANALYZE"), client: "b", ..A },
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 'cs';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[&[T("5")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
