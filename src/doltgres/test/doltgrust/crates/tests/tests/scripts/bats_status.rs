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

//! Dolt's integration-tests/bats/status.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_status() {
    run_scripts(&[
        ScriptTest {
            name: "status: no changes",
            intent: "A fresh database and one whose changes are committed have a clean status on main.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO  test VALUES (0),(1),(2);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
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
                    query: "SELECT dolt_commit('-m', 'new table');",
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "status: staged, unstaged, untracked, ignored tables",
            intent: "dolt_status lists a staged modified table, an unstaged modified table, and a new table, and leaves out an ignored one.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "CREATE TABLE u (pk int PRIMARY KEY);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'tables t, u');",
                "INSERT INTO  t VALUES (1),(2),(3);",
                "INSERT INTO  u VALUES (1),(2),(3);",
                "CREATE TABLE v (pk int PRIMARY KEY);",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                            &[T("public.u"), T("f"), T("modified")],
                            &[T("public.v"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: deleted table",
            intent: "A dropped table shows as deleted, staged once added.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "CREATE TABLE u (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'tables t, u');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE t, u;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table rm drops the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("deleted")],
                            &[T("public.u"), T("f"), T("deleted")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: checkout current branch",
            intent: "Checking out the current branch reports it is already checked out.",
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
            name: "status: tables in conflict",
            intent: "A merge that conflicts leaves the table in conflict in dolt_status.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = on;",
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO t VALUES (1,1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'created table t');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (2,12);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added values on branch other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2,2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added values on branch main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: renamed table",
            intent: "A renamed table shows as a deleted and a new table, and as renamed once staged.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added table test');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME TO quiz;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test -> public.quiz"), T("f"), T("renamed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's dolt_status gives one renamed row, which the bats check reads as the CLI prints it, split into a deleted table and a new one")),
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test -> public.quiz"), T("t"), T("renamed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: unstaged changes after reset",
            intent: "After dolt_reset the working changes stay, unstaged.",
            set_up_script: &[
                "CREATE TABLE one (pk int PRIMARY KEY);",
                "CREATE TABLE two (pk int PRIMARY KEY);",
                "INSERT INTO  one VALUES (0);",
                "INSERT INTO  two VALUES (0);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'create tables one, two');",
                "INSERT INTO  one VALUES (1);",
                "DROP TABLE   two;",
                "CREATE TABLE three (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.one"), T("f"), T("modified")],
                            &[T("public.three"), T("f"), T("new table")],
                            &[T("public.two"), T("f"), T("deleted")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.one"), T("f"), T("modified")],
                            &[T("public.three"), T("f"), T("new table")],
                            &[T("public.two"), T("f"), T("deleted")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: dolt reset --hard <commit-spec>",
            intent: "A hard reset to HEAD^ drops the later commit and the working change.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'made table test');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'inserted 1');",
                "INSERT INTO test VALUES (2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'inserted 2');",
                "INSERT INTO test VALUES (3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'HEAD^');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(pk) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
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
            name: "status: dolt checkout . should result in empty status",
            intent: "dolt_checkout of . discards working changes, and later commits on a new branch keep theirs.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'made table test');",
                "INSERT INTO test VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('.');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(pk) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'test'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
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
                    query: "SELECT dolt_commit('-m', 'insert into test value 2');",
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
                    query: "SELECT sum(pk) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: dolt reset --hard with more than one additional arg throws an error",
            intent: "A hard reset refuses more than one revision.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'HEAD', 'HEAD2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--hard supports at most one additional param", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: dolt reset hard with ~ works",
            intent: "Hard resets to HEAD~1 and HEAD~2 restore those commits' data and a clean status.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm1');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_commit('-am', 'cm2');",
                "INSERT INTO test VALUES (2);",
                "SELECT dolt_commit('-am', 'cm3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(pk) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '-m', 'cm3');",
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
                    query: "SELECT dolt_reset('--hard', 'HEAD~2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(pk) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "status: dolt reset soft with ~ works",
            intent: "Soft resets move the branch back and keep the data, leaving the table untracked once its creating commit is gone.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm1');",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_commit('-am', 'cm2');",
                "INSERT INTO test VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('HEAD~');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(pk) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
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
                    query: "SELECT dolt_reset('HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(pk) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
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
            name: "status: dolt reset with a renamed table",
            intent: "A hard reset undoes a table's rename.",
            set_up_script: &[
                "CREATE TABLE one (pk bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'added table');",
                "ALTER TABLE one RENAME TO one_super;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
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
            name: "status: dolt reset works with commit hash ref",
            intent: "Soft resets to a commit's hash keep later tables as untracked and move the log back.",
            set_up_script: &[
                "CREATE TABLE tb1 (pk int PRIMARY KEY);",
                "INSERT INTO tb1 VALUES (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.cm1', (SELECT commit_hash FROM dolt_log LIMIT 1), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb2 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tb2 VALUES (11);",
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
                    query: "SELECT dolt_commit('-a', '-m', 'cm2');",
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
                    query: "SELECT set_config('test.cm2', (SELECT commit_hash FROM dolt_log LIMIT 1), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tb3 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tb3 VALUES (11);",
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
                    query: "SELECT dolt_commit('-a', '-m', 'cm3');",
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
                    query: "SELECT set_config('test.cm3', (SELECT commit_hash FROM dolt_log LIMIT 1), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset(current_setting('test.cm3'));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset(current_setting('test.cm2'));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.tb3"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM tb3;",
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
                    query: "SELECT count(*) FROM dolt_log;",
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
                    query: "SELECT dolt_commit('-a', '-m', 'commit 3');",
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
                    query: "SELECT dolt_reset(current_setting('test.cm1'));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.tb2"), T("f"), T("new table")],
                            &[T("public.tb3"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
        ScriptTest {
            name: "status: dolt reset works with branch ref",
            intent: "A soft reset to a branch's head leaves the differences as working changes.",
            set_up_script: &[
                "CREATE TABLE tbl(pk int);",
                "SELECT dolt_add('.');",
                "INSERT INTO tbl VALUES (1);",
                "SELECT dolt_commit('-am', 'cm1');",
                "SELECT * FROM dolt_checkout('-b', 'test');",
                "INSERT INTO tbl VALUES (2),(3);",
                "CREATE TABLE tbl2(pk int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'test cm1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('test');",
                "INSERT INTO tbl VALUES (4);",
                "SELECT dolt_commit('-am', 'cm2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.tbl"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: dolt reset ref properly manages staged changes as well",
            intent: "A reset to HEAD unstages staged changes.",
            set_up_script: &[
                "CREATE TABLE tbl(pk int);",
                "SELECT dolt_add('.');",
                "INSERT INTO tbl VALUES (1);",
                "SELECT dolt_commit('-am', 'cm1');",
                "INSERT INTO tbl VALUES (2);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.tbl"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: dolt reset throws errors for unknown ref/table",
            intent: "A reset to a name that is neither a revision nor a table is refused.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: test", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: ignored tables show up",
            intent: "dolt_status_ignored lists the ignored new table beside the others.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "CREATE TABLE u (pk int PRIMARY KEY);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'tables t, u');",
                "INSERT INTO  t VALUES (1),(2),(3);",
                "INSERT INTO  u VALUES (1),(2),(3);",
                "CREATE TABLE v (pk int PRIMARY KEY);",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status_ignored;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.generated_foo"), T("f"), T("new table"), T("t")],
                            &[T("public.t"), T("t"), T("modified"), T("f")],
                            &[T("public.u"), T("f"), T("modified"), T("f")],
                            &[T("public.v"), T("f"), T("new table"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: tables with no observable changes don't show in status but can be staged",
            intent: "A table dropped and recreated identically shows no change and no diff, yet staging it shows it modified.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "INSERT INTO t1 VALUES (1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'create table t1');",
                "CREATE TABLE temp__t1 (id int PRIMARY KEY);",
                "INSERT INTO temp__t1 VALUES (1);",
                "DROP TABLE t1;",
                "ALTER TABLE temp__t1 RENAME TO t1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres keeps a renamed table's constraint names, so the recreated t1's primary key and not-null constraints are named after temp__t1, a difference of its definition that Dolt, whose primary keys and not-null columns have no names, cannot have")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', 't1');",
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
                    query: "SELECT dolt_add('t1');",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: --use-db without branch revision resolves active branch",
            intent: "A connection to the database, or to a branch of it, reports that branch and its working changes.",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'init');",
                "INSERT INTO t VALUES (1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db",
                    database: "postgres",
                    dolt: Some(Dolt::MySqlOnly("dolt --use-db names the database or branch the command runs in, as a connection's login does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    database: "postgres/main",
                    dolt: Some(Dolt::MySqlOnly("dolt --use-db names the database or branch the command runs in, as a connection's login does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "main",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "other",
                    database: "postgres/other",
                    dolt: Some(Dolt::MySqlOnly("dolt --use-db names the database or branch the command runs in, as a connection's login does")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: ALTER TABLE AUTO_INCREMENT value change shows table as modified",
            intent: "Moving an identity column's next value shows the table modified until it is committed.",
            set_up_script: &[
                "CREATE TABLE ainc (id int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY);",
                "INSERT INTO ainc DEFAULT VALUES;",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'create ainc');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ainc ALTER COLUMN id RESTART WITH 100;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's AUTO_INCREMENT = 100 restarts the identity column's sequence")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.ainc"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('ainc');",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.ainc"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'bump auto_increment');",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "status: with remote",
            intent: "A clone's main tracks origin/main, and is up to date with it, behind it after a fetch, diverged from it after a local commit, and ahead of it after a reset to it and a commit.",
            set_up_script: &[
                "CREATE DATABASE repo1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('origin', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, remote, branch FROM dolt_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T("main"), T("origin"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly("dolt status's upstream text is the CLI's, so the port reads dolt_branch_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('remotes/origin/main', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("main"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly("dolt status's upstream text is the CLI's, so the port reads dolt_branch_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'cm1') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('origin', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_fetch('origin');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_fetch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('remotes/origin/main', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("main"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly("dolt status's upstream text is the CLI's, so the port reads dolt_branch_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'cm2') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('remotes/origin/main', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("main"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly("dolt status's upstream text is the CLI's, so the port reads dolt_branch_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'origin/main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'cm3') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('remotes/origin/main', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T("main"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly("dolt status's upstream text is the CLI's, so the port reads dolt_branch_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "status: after remove remote",
            intent: "A clone's status still works after its origin remote is removed.",
            set_up_script: &[
                "CREATE DATABASE repo1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('origin', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, url FROM dolt_remotes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("url", TEXT)],
                        rows: &[
                            &[T("origin"), T("file://{NEWDIR:remote}")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('remove', 'origin');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly("dolt status's branch line is the CLI's, so the port reads active_branch and dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
