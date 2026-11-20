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

//! Dolt's integration-tests/bats/sql-clean.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_clean() {
    run_scripts(&[
        ScriptTest {
            name: r#"sql-clean: DOLT_CLEAN clears unstaged tables"#,
            intent: r#"dolt_clean drops new tables that aren't staged, all of them or those named, changes nothing with --dry-run, and leaves staged tables alone."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-a', '-m', 'Add a table');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test2 (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clean();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test2 (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clean('--dry-run');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clean('test2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test2 (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test3 (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clean('test3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt clean is a CLI command, which dolt_clean() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test2');"#,
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
                    query: r#"SELECT dolt_clean();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt clean is a CLI command, which dolt_clean() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-clean: DOLT_CLEAN unknown table name"#,
            intent: r#"dolt_clean of a table that doesn't exist fails and leaves the new tables as they were."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-a', '-m', 'Add a table');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test2 (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clean('unknown');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"failed to clean; table not found: 'unknown'"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clean('unknown');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"failed to clean; table not found: 'unknown'"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt clean is a CLI command, which dolt_clean() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
