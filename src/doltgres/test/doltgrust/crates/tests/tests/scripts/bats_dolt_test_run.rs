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

//! Dolt's integration-tests/bats/dolt-test-run.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_test_run() {
    run_scripts(&[
        ScriptTest {
            name: r#"dolt-test-run: sanity test on sql-server"#,
            intent: r#"dolt_test_run runs the tests dolt_tests holds and reports them passing."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('test1', 'test1', 'select 1', 'expected_rows', '==', '1');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES ('test2', 'test2', 'select 2', 'expected_single_value', '==', '2');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT test_name, test_group_name, query, status, message FROM dolt_test_run() ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("query", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"test1"#), T(r#"test1"#), T(r#"select 1"#), T(r#"PASS"#), T(r#""#)],
                            &[T(r#"test2"#), T(r#"test2"#), T(r#"select 2"#), T(r#"PASS"#), T(r#""#)],
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
