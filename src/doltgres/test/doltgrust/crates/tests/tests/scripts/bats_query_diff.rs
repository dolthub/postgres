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

//! Dolt's integration-tests/bats/query-diff.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_query_diff() {
    run_scripts(&[
        ScriptTest {
            name: "query-diff: no args",
            intent: "dolt_query_diff refuses a call without its two queries.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE TABLE tt (i int PRIMARY KEY, j int);",
                "INSERT INTO tt VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET j = 10 WHERE i = 2;",
                "DELETE FROM t WHERE i = 3;",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_query_diff() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "query-diff: too many args",
            intent: "dolt_query_diff refuses a call with more than two queries.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE TABLE tt (i int PRIMARY KEY, j int);",
                "INSERT INTO tt VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET j = 10 WHERE i = 2;",
                "DELETE FROM t WHERE i = 3;",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('SELECT * FROM t', 'SELECT * FROM t', 'SELECT * FROM t');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function dolt_query_diff(unknown, unknown, unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    dolt: Some(Dolt::Differs("the diff functions take any argument Postgres can evaluate before the query runs and resolve by argument types, Appendix C \"arguments of the diff functions\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "query-diff: no changes",
            intent: "dolt_query_diff of a query with itself has no rows.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE TABLE tt (i int PRIMARY KEY, j int);",
                "INSERT INTO tt VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET j = 10 WHERE i = 2;",
                "DELETE FROM t WHERE i = 3;",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('SELECT * FROM t', 'SELECT * FROM t');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "query-diff: basic case",
            intent: "dolt_query_diff shows the rows two revisions of a table differ in, by key.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE TABLE tt (i int PRIMARY KEY, j int);",
                "INSERT INTO tt VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET j = 10 WHERE i = 2;",
                "DELETE FROM t WHERE i = 3;",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('SELECT * FROM t AS OF other', 'SELECT * FROM t AS OF HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("2"), T("2"), T("10"), T("modified")],
                            &[T("3"), T("3"), Null, Null, T("deleted")],
                            &[Null, Null, T("4"), T("4"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "query-diff: other table",
            intent: "dolt_query_diff compares the rows of two different tables by key.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE TABLE tt (i int PRIMARY KEY, j int);",
                "INSERT INTO tt VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET j = 10 WHERE i = 2;",
                "DELETE FROM t WHERE i = 3;",
                "INSERT INTO t VALUES (4, 4);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('SELECT * FROM t', 'SELECT * FROM tt');",
                    expected: Expected::Rows {
                        columns: &[Column("from_i", INT4), Column("from_j", INT4), Column("to_i", INT4), Column("to_j", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), Null, Null, T("deleted")],
                            &[T("2"), T("10"), Null, Null, T("deleted")],
                            &[T("4"), T("4"), Null, Null, T("deleted")],
                            &[Null, Null, T("1"), T("1"), T("added")],
                            &[Null, Null, T("2"), T("2"), T("added")],
                            &[Null, Null, T("3"), T("3"), T("added")],
                        ],
                        tag: "SELECT 6",
                    },
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
