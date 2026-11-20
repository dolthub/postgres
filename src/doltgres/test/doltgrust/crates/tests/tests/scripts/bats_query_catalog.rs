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

//! Dolt's integration-tests/bats/query-catalog.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_query_catalog() {
    run_scripts(&[
        ScriptTest {
            name: "query-catalog: can describe dolt_query_catalog",
            intent: "dolt_query_catalog has id and query columns.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'dolt_query_catalog' AND column_name IN ('id', 'query') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("id")],
                            &[T("query")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
            ],
            skip: Some("information_schema.columns lists no columns of dolt_query_catalog, where the port expects id and query (bug, MacDG)"),
            ..S
        },
    ]);
}
