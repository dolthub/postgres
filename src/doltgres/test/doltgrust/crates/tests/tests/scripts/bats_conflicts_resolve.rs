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

//! Dolt's integration-tests/bats/conflicts-resolve.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_conflicts_resolve() {
    run_scripts(&[
        ScriptTest {
            name: "conflicts-resolve: call with no arguments, errors",
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
            name: "conflicts-resolve: call without specifying table, errors",
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
            name: "conflicts-resolve: call with non-existent table, errors",
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
            name: "conflicts-resolve: no conflicts, no changes",
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
            name: "conflicts-resolve: merge other into main, resolve with ours",
            intent: "Resolving every table's conflict with --ours keeps our row.",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', '.');",
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
            name: "conflicts-resolve: merge other into main, resolve with ours, specify table",
            intent: "Resolving the table's conflict with --ours keeps our row.",
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
            name: "conflicts-resolve: merge other into main, resolve with theirs",
            intent: "Resolving every table's conflict with --theirs keeps their row.",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', '.');",
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
            name: "conflicts-resolve: merge other into main, resolve with theirs, specify table",
            intent: "Resolving the table's conflict with --theirs keeps their row.",
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
            name: "conflicts-resolve: two conflicted tables, resolve theirs all",
            intent: "Resolving every table with --theirs takes their rows in both tables and leaves no conflicts.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, t text);",
                "CREATE TABLE t2 (i int PRIMARY KEY, t text);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 'other');",
                "INSERT INTO t2 VALUES (1, 'other2');",
                "SELECT dolt_commit('-am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main');",
                "INSERT INTO t2 VALUES (1, 'main2');",
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
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main2")],
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
                    query: "SELECT * FROM dolt_conflicts ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                            &[T("public.t2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', '.');",
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
                    query: "SELECT * FROM dolt_conflicts ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
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
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("other2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicts-resolve: two conflicted tables, resolve theirs one table, ours other table",
            intent: "Each table resolves with its own side, and then no conflicts are left.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, t text);",
                "CREATE TABLE t2 (i int PRIMARY KEY, t text);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'init commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 'other');",
                "INSERT INTO t2 VALUES (1, 'other2');",
                "SELECT dolt_commit('-am', 'other commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main');",
                "INSERT INTO t2 VALUES (1, 'main2');",
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
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main2")],
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
                    query: "SELECT * FROM dolt_conflicts ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                            &[T("public.t2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
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
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't2');",
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
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("main2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
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
            name: "conflicts-resolve: merge main into other, resolve with ours",
            intent: "Resolving every table's conflict with --ours keeps our row.",
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
                    query: "SELECT dolt_conflicts_resolve('--ours', '.');",
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
            name: "conflicts-resolve: merge main into other, resolve with ours, specify table",
            intent: "Resolving the table's conflict with --ours keeps our row.",
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
            name: "conflicts-resolve: merge main into other, resolve with theirs",
            intent: "Resolving every table's conflict with --theirs keeps their row.",
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', '.');",
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
            name: "conflicts-resolve: merge main into other, resolve with theirs, specify table",
            intent: "Resolving the table's conflict with --theirs keeps their row.",
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
