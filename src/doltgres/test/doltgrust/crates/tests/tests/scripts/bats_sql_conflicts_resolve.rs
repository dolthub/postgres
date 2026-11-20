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

//! Dolt's integration-tests/bats/sql-conflicts-resolve.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_conflicts_resolve() {
    run_scripts(&[
        ScriptTest {
            name: "sql-conflicts-resolve: call with no arguments, errors",
            intent: "dolt_conflicts_resolve needs --ours or --theirs.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--ours or --theirs must be supplied", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts-resolve: call without specifying table, errors",
            intent: "dolt_conflicts_resolve needs at least one table.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "specify at least one table to resolve conflicts", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts-resolve: call with non-existent table, errors",
            intent: "dolt_conflicts_resolve refuses a table that does not exist.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 'notexists');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "table not found", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts-resolve: no conflicts, no changes",
            intent: "Without conflicts, resolving with either side changes no table.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, t text);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 'other');",
                "SELECT dolt_commit('-am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main');",
                "SELECT dolt_commit('-am', 'main commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("other")],
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts-resolve: merge other into main, resolve with ours",
            intent: "Resolving a merge's conflict with --ours keeps our row.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, t text);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 'other');",
                "SELECT dolt_commit('-am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main');",
                "SELECT dolt_commit('-am', 'main commit');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts-resolve: merge other into main, resolve with theirs",
            intent: "Resolving a merge's conflict with --theirs keeps their row.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, t text);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 'other');",
                "SELECT dolt_commit('-am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main');",
                "SELECT dolt_commit('-am', 'main commit');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts-resolve: merge main into other, resolve with ours",
            intent: "Resolving a merge's conflict with --ours keeps our row.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, t text);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 'other');",
                "SELECT dolt_commit('-am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main');",
                "SELECT dolt_commit('-am', 'main commit');",
                "SELECT * FROM dolt_checkout('other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-conflicts-resolve: merge main into other, resolve with theirs",
            intent: "Resolving a merge's conflict with --theirs keeps their row.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, t text);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 'other');",
                "SELECT dolt_commit('-am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main');",
                "SELECT dolt_commit('-am', 'main commit');",
                "SELECT * FROM dolt_checkout('other');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main")],
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
