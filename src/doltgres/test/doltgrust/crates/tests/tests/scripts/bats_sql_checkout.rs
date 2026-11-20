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

//! Dolt's integration-tests/bats/sql-checkout.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_checkout() {
    run_scripts(&[
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT just works",
            intent: "dolt_checkout -b makes a branch and switches to it, and dolt_checkout switches back.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("feature-branch")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT -b throws error on branches that already exist",
            intent: "dolt_checkout -b refuses a branch name that is taken.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'main' already exists.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT throws error on branches that don't exist",
            intent: "dolt_checkout of a name that is neither a branch nor a table fails.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: tablespec 'feature-branch' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT -b throws error on empty branch",
            intent: "dolt_checkout -b refuses an empty name.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: cannot checkout empty string", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT changes branches, leaves behind working set unmodified.",
            intent: "Each branch keeps its own uncommitted changes when the session switches between them, and a new branch starts from its start point's commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', '0, 1, and 2 in test table');",
                "INSERT INTO test VALUES (4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("feature-branch")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'feature-branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature-branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT works with dolt_diff tables",
            intent: "On a branch dolt_checkout -b makes, dolt_diff_test shows what it showed after the commit, the commit's three added rows, whatever the branch it left has uncommitted.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', '1, 2, and 3 in test table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, from_pk, dolt_diff_type FROM dolt_diff_test ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("from_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), Null, T("added")],
                            &[T("1"), Null, T("added")],
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, from_pk, dolt_diff_type FROM dolt_diff_test ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("from_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), Null, T("added")],
                            &[T("1"), Null, T("added")],
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) > 0 FROM dolt_diff_test;",
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
                    query: "SELECT * FROM dolt_checkout('-b', 'feature-branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, from_pk, dolt_diff_type FROM dolt_diff_test ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("from_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), Null, T("added")],
                            &[T("1"), Null, T("added")],
                            &[T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT followed by DOLT_COMMIT",
            intent: "A commit made after dolt_checkout -b lands on the new branch with its author, leaving main's head alone.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', '0, 1, and 2 in test table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Added 4', '--author', 'John Doe <john@doe.com>');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("0, 1, and 2 in test table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT)],
                        rows: &[
                            &[T("Added 4"), T("John Doe")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT)],
                        rows: &[
                            &[T("Added 4"), T("John Doe")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT with table name clears working set changes",
            intent: "dolt_checkout of a table gives the session's branch HEAD's rows of it back.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', '0, 1, and 2 in test table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('test');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT between branches operating on the same table works.",
            intent: "Two branches commit different rows to the same table, and each keeps its own.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT does not throw an error when checking out to the same branch",
            intent: "dolt_checkout of the session's own branch succeeds.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Already on branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT with --no-overwrite-ignore aborts when ignored table would be overwritten",
            intent: "dolt_checkout --no-overwrite-ignore refuses to switch when an ignored table would be overwritten.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'setup ignored table on main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table on other', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--no-overwrite-ignore', 'other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "The following ignored tables would be overwritten by checkout:
	ignored_tbl
Please move or remove them before you switch branches.
Use --overwrite-ignore to force.
", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT with --overwrite-ignore succeeds",
            intent: "dolt_checkout --overwrite-ignore switches even when an ignored table would be overwritten.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'setup ignored table on main');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table on other', '--force');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--overwrite-ignore', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT --overwrite-ignore and --no-overwrite-ignore are mutually exclusive",
            intent: "dolt_checkout refuses --overwrite-ignore with --no-overwrite-ignore.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('--overwrite-ignore', '--no-overwrite-ignore', 'other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: --overwrite-ignore and --no-overwrite-ignore are mutually exclusive", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT -b with --no-overwrite-ignore aborts when ignored table differs at start point",
            intent: "dolt_checkout -b --no-overwrite-ignore refuses when an ignored table differs at the start point.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on main', '--force');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table', '--force');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newbranch', '--no-overwrite-ignore', 'HEAD~1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "The following ignored tables would be overwritten by checkout:
	ignored_tbl
Please move or remove them before you switch branches.
Use --overwrite-ignore to force.
", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT -b with --overwrite-ignore succeeds when ignored table differs at start point",
            intent: "dolt_checkout -b --overwrite-ignore switches when an ignored table differs at the start point.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on main', '--force');",
                "INSERT INTO ignored_tbl VALUES (2, 200);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'modify ignored table', '--force');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newbranch', '--overwrite-ignore', 'HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'newbranch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-checkout: DOLT_CHECKOUT -b with --no-overwrite-ignore from HEAD succeeds (hashes identical)",
            intent: "dolt_checkout -b --no-overwrite-ignore from HEAD switches, since no ignored table can differ there.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "CREATE TABLE ignored_tbl (pk int PRIMARY KEY, val int);",
                "INSERT INTO ignored_tbl VALUES (1, 100);",
                "INSERT INTO dolt_ignore VALUES ('ignored_tbl', TRUE);",
                "SELECT dolt_add('-A', '--force');",
                "SELECT dolt_commit('-m', 'add ignored table on main', '--force');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'newbranch', '--no-overwrite-ignore');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'newbranch'")],
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
