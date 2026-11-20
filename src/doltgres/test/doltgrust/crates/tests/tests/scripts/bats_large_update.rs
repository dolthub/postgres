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

//! Dolt's integration-tests/bats/large-update.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_large_update() {
    run_scripts(&[
        ScriptTest {
            name: r#"large-update: run a large update"#,
            intent: r#"An update of every row of a 262144-row table succeeds."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (id INT PRIMARY KEY, val REAL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's FLOAT is Postgres' REAL"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT g, 0.0 FROM generate_series(0, 15) g;"#,
                    expected: Expected::Tag("INSERT 0 16"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 16"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 32"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 64"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 128"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 256"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 1024"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 4096"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 8192"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 16384"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 32768"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 65536"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id, val) SELECT id + (SELECT count(*) FROM test), 0.0 FROM test;"#,
                    expected: Expected::Tag("INSERT 0 131072"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET val = val + 0.001;"#,
                    expected: Expected::Tag("UPDATE 262144"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"262144"#)],
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
