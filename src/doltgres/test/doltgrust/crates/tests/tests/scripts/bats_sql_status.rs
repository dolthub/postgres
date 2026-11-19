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

//! Dolt's integration-tests/bats/sql-status.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_status() {
    run_scripts(&[
        ScriptTest {
            name: "sql-status: status properly works with working and staged tables",
            intent: "A new table shows unstaged in dolt_status, then staged once added.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
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
                            &[T("public.test"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-status: status properly works with table rename",
            intent: "Renaming a staged new table shows the rename unstaged beside the staged new table, and staging it shows the new name staged.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
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
                            &[T("public.test"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME TO test2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("new table")],
                            &[T("public.test -> public.test2"), T("f"), T("renamed")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test2');",
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
                            &[T("public.test2"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-status: table that has staged and unstaged changes shows up twice",
            intent: "A table with staged and unstaged changes has a row for each.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
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
                    query: "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("new table")],
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-status: status properly works with docs",
            intent: "Docs written to dolt_docs show dolt_docs as a new table.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_docs VALUES ('README.md', 'readme-text'), ('LICENSE.md', 'license-text');",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload writes the file into dolt_docs")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_docs"), T("f"), T("new table")],
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-status: status works property with working tables in conflict",
            intent: "A merge that conflicts shows the table in conflict.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = on;",
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'table created');",
                "SELECT dolt_branch('change-cell');",
                "INSERT INTO test VALUES (0, 1, 1, 1, 1, 1) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=0 all cells to 1');",
                "SELECT * FROM dolt_checkout('change-cell');",
                "INSERT INTO test VALUES (0, 11, 11, 11, 11, 11) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'changed pk=0 all cells to 11');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('change-cell', '-m', 'merge change-cell');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-status: status works properly with working docs in conflict",
            intent: "Docs changed on two branches merge fast-forward from one and conflict with the other, leaving dolt_docs in conflict.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO dolt_docs VALUES ('README.md', 'a readme');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Committing initial docs');",
                "SELECT dolt_branch('test-a');",
                "SELECT dolt_branch('test-b');",
                "SELECT * FROM dolt_checkout('test-a');",
                "UPDATE dolt_docs SET doc_text = 'test-a branch' WHERE doc_name = 'README.md';",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'Changed README.md on test-a branch');",
                "SELECT * FROM dolt_checkout('test-b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT doc_text FROM dolt_docs WHERE doc_name = 'README.md';",
                    expected: Expected::Rows {
                        columns: &[Column("doc_text", TEXT)],
                        rows: &[
                            &[T("a readme")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_docs SET doc_text = 'test-b branch' WHERE doc_name = 'README.md';",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload writes the file into dolt_docs")),
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
                    query: "SELECT dolt_commit('-m', 'Changed README.md on test-a branch');",
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
                    query: "SELECT * FROM dolt_merge('test-a');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT doc_text FROM dolt_docs WHERE doc_name = 'README.md';",
                    expected: Expected::Rows {
                        columns: &[Column("doc_text", TEXT)],
                        rows: &[
                            &[T("test-a branch")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test-b', '-m', 'merge test-b');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_docs"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("the merge's conflicts roll back an autocommit statement, since Doltgrust resolves conflicts only inside an explicit transaction; the port needs BEGIN or dolt_allow_commit_conflicts"),
            ..S
        },
        ScriptTest {
            name: "sql-status: tables with no observable changes don't show in status but can be staged",
            intent: "A table dropped and recreated identically shows no status and no diff, yet staging it shows it modified.",
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
                    query: "SELECT count(*) = 0 FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
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
    ]);
}
