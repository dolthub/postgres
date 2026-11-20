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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_clean.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_clean_procedure_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_clean does not drop tables matching dolt_ignore",
            intent: "dolt_clean leaves untracked tables that dolt_ignore names.",
            set_up_script: &[
                "CREATE TABLE ignored_foo (id int PRIMARY KEY);",
                "INSERT INTO ignored_foo VALUES (1);",
                "INSERT INTO dolt_ignore VALUES ('ignored_*', TRUE);",
                "SELECT dolt_add('dolt_ignore');",
                "SELECT dolt_commit('-m', 'add dolt_ignore');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM ignored_foo;",
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
                    query: "SELECT dolt_clean();",
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
                    query: "SELECT * FROM ignored_foo;",
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
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("ignored_foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_clean -x drops tables matching dolt_ignore",
            intent: "dolt_clean -x drops untracked tables even when dolt_ignore names them.",
            set_up_script: &[
                "CREATE TABLE ignored_bar (id int PRIMARY KEY);",
                "INSERT INTO ignored_bar VALUES (1);",
                "INSERT INTO dolt_ignore VALUES ('ignored_*', TRUE);",
                "SELECT dolt_add('dolt_ignore');",
                "SELECT dolt_commit('-m', 'add dolt_ignore');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM ignored_bar;",
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
                    query: "SELECT dolt_clean('-x');",
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
                    query: "SELECT * FROM ignored_bar;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"ignored_bar\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
