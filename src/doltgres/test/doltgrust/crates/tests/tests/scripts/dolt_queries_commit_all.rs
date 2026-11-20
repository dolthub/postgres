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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_commit_all.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_commit_all_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_commit_all publishes three heads and preserves selected database",
            intent: "dolt_commit_all commits every branch the transaction changed, each with the message, and leaves the session on its branch.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "BEGIN;",
                "INSERT INTO \"postgres/main\".public.t VALUES (2, 20);",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'all branches');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/main\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("all branches")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("all branches")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b2\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("all branches")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT CURRENT_SCHEMA(), active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("current_schema", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("postgres"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3, 30);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("the Doltgres expectation names the current schema after the database, where Postgres' is public"),
            ..S
        },
        ScriptTest {
            name: "dolt_commit retains its single dirty branch restriction",
            intent: "dolt_commit fails and commits nothing when the transaction changed more than one branch, while dolt_commit_all commits them all.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "BEGIN;",
                "INSERT INTO \"postgres/main\".public.t VALUES (2, 20);",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'single');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'all branches');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_commit_all keeps unstaged changes on every branch",
            intent: "dolt_commit_all without -a commits only each branch's staged changes and leaves the unstaged ones in the working sets.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "BEGIN;",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (3, 30);",
                "SELECT * FROM dolt_checkout('b1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (3, 30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-m', 'staged only');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_commit_all preparation failure leaves every head unchanged",
            intent: "When one changed branch has nothing staged, dolt_commit_all fails naming it and moves no branch's head.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "BEGIN;",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT dolt_add('.');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-m', 'fails');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "nothing to commit on postgres/main", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT CURRENT_SCHEMA();",
                    expected: Expected::Rows {
                        columns: &[Column("current_schema", NAME)],
                        rows: &[
                            &[T("postgres/main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'retry');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_commit_all's failed preparation fails with 55000, where the port expects 22023"),
            ..S
        },
        ScriptTest {
            name: "dolt_commit_all skip-empty still publishes skipped working sets",
            intent: "With --skip-empty, dolt_commit_all commits the branches with staged changes and keeps the others' working changes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "BEGIN;",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT dolt_add('.');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('--skip-empty', '-m', 'staged branch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_commit_all amends each branch independently",
            intent: "dolt_commit_all --amend replaces each changed branch's head commit, so no branch's history grows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "BEGIN;",
                "INSERT INTO \"postgres/main\".public.t VALUES (2, 20);",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.before_count', ((SELECT count(*) FROM dolt_log))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('--amend', '-am', 'amended');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) = current_setting('test.before_count')::bigint FROM \"postgres/main\".pg_catalog.dolt_log;",
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
                    query: "SELECT count(*) = current_setting('test.before_count')::bigint FROM \"postgres/b1\".pg_catalog.dolt_log;",
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
                    query: "SELECT count(*) = current_setting('test.before_count')::bigint FROM \"postgres/b2\".pg_catalog.dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge requires every working set to be transactionally clean",
            intent: "dolt_merge fails while the transaction has uncommitted changes on another branch, and merges once they are committed.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_commit('-am', 'advance b1');",
                "SELECT * FROM dolt_checkout('main');",
                "BEGIN;",
                "INSERT INTO \"postgres/b2\".public.t VALUES (3, 30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_merge of b1 succeeds although another working set has pending edits, where this test expects the refusal (22023) (bug, MacDG)"),
            ..S
        },
        ScriptTest {
            name: "dolt_commit_all with no dirty branches uses the selected branch",
            intent: "With no changed branch, dolt_commit_all --allow-empty commits the current branch, --skip-empty commits nothing, and --branch is refused.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('--allow-empty', '-m', 'empty');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('--skip-empty', '-m', 'skip');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('--branch', 'b1', '-am', 'unsupported');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--branch is not supported by dolt_commit_all", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_commit_all_transaction_tests() {
    run_scripts(&[
        ScriptTest {
            name: "concurrent dolt_commit_all merges every head",
            intent: "Two transactions' dolt_commit_all calls over the same branches both succeed, the second merging its rows into the heads the first made.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'b');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("the concurrent script never ends: a client waits on another's open transaction and times out after 120 s; re-port its concurrency to Postgres' row locks"),
            ..S
        },
        ScriptTest {
            name: "conflict in dolt_commit_all leaves all other heads unchanged",
            intent: "When another transaction committed a conflicting change to one branch, dolt_commit_all fails with a serialization failure and moves no branch.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 10;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 20;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'a');",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("the concurrent script never ends: client b's UPDATE of the row client a updated waits for client a's row lock and times out after 120 s; re-port its concurrency to Postgres' row locks"),
            ..S
        },
        ScriptTest {
            name: "stale amend aborts every head in dolt_commit_all",
            intent: "dolt_commit_all --amend fails when another transaction moved one of the heads it would amend, and moves no branch.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('--amend', '-am', 'a');",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("the concurrent script never ends: a client waits on another's open transaction and times out after 120 s; re-port its concurrency to Postgres' row locks"),
            ..S
        },
        ScriptTest {
            name: "merge rejects other branch edits without changing heads",
            intent: "dolt_merge fails while the transaction has changes on another branch, leaving that branch's rows and every head unchanged.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_commit('-am', 'advance');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (4, 40);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("dolt_merge of b1 succeeds although another working set has pending edits, where this test expects the refusal (22023) (bug, MacDG)"),
            ..S
        },
        ScriptTest {
            name: "merge rejection preserves other staged edits for a later commit",
            intent: "A refused dolt_merge leaves the transaction's staged changes on another branch for a later dolt_commit there.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_commit('-am', 'advance');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot merge with uncommitted changes", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (4, 40);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b2');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'pending staged edits');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("3"), T("30")],
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("dolt_merge of b1 succeeds although another working set has pending edits, where this test expects the refusal (22023) (bug, MacDG)"),
            ..S
        },
    ]);
}
