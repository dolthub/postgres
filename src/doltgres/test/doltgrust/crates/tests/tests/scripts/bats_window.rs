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

//! Dolt's integration-tests/bats/window.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_window() {
    run_scripts(&[
        ScriptTest {
            name: r#"window: no diff table window buffer sharing"#,
            intent: r#"Window functions over a diff table each see their own partitions and order."#,
            set_up_script: &[
                r#"CREATE TABLE a (x int PRIMARY KEY, y varchar(1));"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'table');"#,
                r#"INSERT INTO a VALUES (1, 'a'), (0, 'b'), (2, 'c');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'row');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT to_x, first_value(to_y) OVER (PARTITION BY to_x), row_number() OVER (PARTITION BY dolt_diff_type ORDER BY to_x DESC) FROM dolt_diff_a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_x", INT4), Column("first_value", VARCHAR), Column("row_number", INT8)],
                        rows: &[
                            &[T("0"), T("b"), T("3")],
                            &[T("1"), T("a"), T("2")],
                            &[T("2"), T("c"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_diff_a names Dolt's diff_type column dolt_diff_type")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
