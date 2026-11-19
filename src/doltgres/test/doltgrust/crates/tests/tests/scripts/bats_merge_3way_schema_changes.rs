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

//! Dolt's integration-tests/bats/merge-3way-schema-changes.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_merge_3way_schema_changes() {
    run_scripts(&[
        ScriptTest {
            name: "merge-3way-schema-changes: add a NOT NULL column with default value on a branch",
            intent: "A NOT NULL column with a default added on one side fills the other side's new rows with the default.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'ancestor');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (1);",
                "ALTER TABLE t ADD COLUMN col1 int NOT NULL DEFAULT 0;",
                "SELECT dolt_commit('-am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'left');",
                "SELECT * FROM dolt_merge('right');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge-3way-schema-changes: migrate a schema merge too large to fit in a MutableMap",
            intent: "A column type change on one side merges with an update of every row of a 70,000-row table on the other.",
            set_up_script: &[
                "CREATE TABLE test_table (pk int PRIMARY KEY, t varchar(20));",
                "INSERT INTO test_table SELECT i, 'hello world' FROM generate_series(0, 69999) AS i;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table');",
                "SELECT dolt_branch('schema_change');",
                "SELECT dolt_branch('data_change');",
                "SELECT * FROM dolt_checkout('schema_change');",
                "ALTER TABLE test_table ALTER COLUMN t TYPE text;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'modify column t to text');",
                "SELECT * FROM dolt_checkout('data_change');",
                "UPDATE test_table SET t = 'new text';",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'update column t');",
                "SELECT * FROM dolt_merge('schema_change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) AS type FROM pg_attribute WHERE attrelid = 'test_table'::regclass AND attname = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT)],
                        rows: &[
                            &[T("text")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test_table WHERE t = 'new text';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("70000")],
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
