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

//! Dolt's integration-tests/bats/column_tags.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_column_tags() {
    run_scripts(&[
        ScriptTest {
            name: r#"column_tags: Merging two branches that added same tag, name, type, and constraints"#,
            intent: r#"Two branches that add the same column merge without conflict."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Committed test table');"#,
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT dolt_branch('branch2');"#,
                r#"SELECT dolt_checkout('branch1');"#,
                r#"ALTER TABLE test ADD COLUMN c2 bigint;"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Added column c2 bigint');"#,
                r#"SELECT dolt_checkout('branch2');"#,
                r#"ALTER TABLE test ADD COLUMN c2 bigint;"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Added column c2 bigint');"#,
                r#"SELECT dolt_checkout('main');"#,
                r#"SELECT dolt_merge('branch1', '-m', 'merge branch1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('branch2', '-m', 'merge branch2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, which dolt_merge() is")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"column_tags: Merging branches that use the same tag referring to different schema fails"#,
            intent: r#"Two branches that add a column of the same name with different types conflict on the table's definition."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Committed test table');"#,
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT dolt_branch('branch2');"#,
                r#"SELECT dolt_checkout('branch1');"#,
                r#"ALTER TABLE test ADD COLUMN c2 bigint;"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Added column c2 bigint');"#,
                r#"SELECT dolt_checkout('branch2');"#,
                r#"ALTER TABLE test ADD COLUMN c2 text;"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Added column c2 longtext');"#,
                r#"SELECT dolt_checkout('main');"#,
                r#"SELECT dolt_merge('branch1');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('branch2', '-m', 'merge branch2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, which dolt_merge() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM dolt_schema_conflicts;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("the CLI's CONFLICT (schema) line is a row of dolt_schema_conflicts")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"column_tags: Merging branches that both created the same column succeeds"#,
            intent: r#"Two branches that add the same two columns merge into a table with each column once."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Committed test table');"#,
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT dolt_branch('branch2');"#,
                r#"SELECT dolt_checkout('branch1');"#,
                r#"ALTER TABLE test ADD COLUMN c2 bigint;"#,
                r#"ALTER TABLE test ADD COLUMN c3 double precision;"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Added columns c2 bigint and c3 double to branch1');"#,
                r#"SELECT dolt_checkout('branch2');"#,
                r#"ALTER TABLE test ADD COLUMN c2 bigint;"#,
                r#"ALTER TABLE test ADD COLUMN c3 double precision;"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Added columns c2 bigint and c3 double to branch2');"#,
                r#"SELECT dolt_checkout('main');"#,
                r#"SELECT dolt_merge('branch1', '-m', 'merge branch1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('branch2', '-m', 'merge branch2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, which dolt_merge() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;"#,
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                            &[T("c1"), T("bigint")],
                            &[T("c2"), T("bigint")],
                            &[T("c3"), T("double precision")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show is a CLI command, so the port reads information_schema.columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"column_tags: Merging branches that both created the same table succeeds"#,
            intent: r#"Two branches that create the same table merge into one table."#,
            set_up_script: &[
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT dolt_branch('branch2');"#,
                r#"SELECT dolt_checkout('branch1');"#,
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Committed test table');"#,
                r#"SELECT dolt_checkout('branch2');"#,
                r#"CREATE TABLE test (pk bigint NOT NULL, c1 bigint, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'Committed test table');"#,
                r#"SELECT dolt_checkout('main');"#,
                r#"SELECT dolt_merge('branch1', '-m', 'merge branch1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('branch2', '-m', 'merge branch2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, which dolt_merge() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;"#,
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                            &[T("c1"), T("bigint")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show is a CLI command, so the port reads information_schema.columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"column_tags: create table on two separate branches, merge them together even though they have different tags"#,
            intent: r#"Two branches that create the same table, one with a column it then drops, merge with both branches' rows."#,
            set_up_script: &[
                r#"SELECT dolt_branch('other');"#,
                r#"CREATE TABLE t (pk int PRIMARY KEY, col1 int);"#,
                r#"INSERT INTO t VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-Am', 'unrelated table');"#,
                r#"CREATE TABLE target (pk int PRIMARY KEY, col1 int);"#,
                r#"INSERT INTO target VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-Am', 'table target on main branch');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"CREATE TABLE target (pk int PRIMARY KEY, badCol int, col1 int);"#,
                r#"INSERT INTO target VALUES (2, 2, 2);"#,
                r#"SELECT dolt_commit('-Am', 'table target on other branch');"#,
                r#"ALTER TABLE target DROP COLUMN badCol;"#,
                r#"SELECT dolt_commit('-Am', 'fixup');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge other into main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, which dolt_merge() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_stat('HEAD~', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[Any, Any, T("1"), Any, Any, Any, Any, Any, Any, Any, Any, Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt merge's summary line is a CLI command's, so the port reads dolt_diff_stat() of the merge commit")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM target ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
