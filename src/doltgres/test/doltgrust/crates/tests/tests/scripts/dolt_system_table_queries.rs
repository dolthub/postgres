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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_system_table_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_backups_system_table_queries() {
    run_scripts(&[
        ScriptTest {
            name: r#"dolt_backups table"#,
            intent: r#"dolt_backups lists the backups dolt_backup adds and removes, and refuses writes."#,
            set_up_script: &[
                r#"SELECT dolt_backup('add', 'backup3', 'file:///tmp/backup3');"#,
                r#"SELECT dolt_backup('add', 'backup1', 'file:///tmp/backup1');"#,
                r#"SELECT dolt_backup('add', 'backup2', 'aws://[ddb_table:ddb_s3_bucket]/db1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_backups ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"backup1"#)],
                            &[T(r#"backup2"#)],
                            &[T(r#"backup3"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT url FROM dolt_backups WHERE name = 'backup2';"#,
                    expected: Expected::Rows {
                        columns: &[Column("url", TEXT)],
                        rows: &[
                            &[T(r#"aws://[ddb_table:ddb_s3_bucket]/db1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_backups WHERE name = 'backup1';"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot delete from view "dolt_backups""#, detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.", ..E }),
                    dolt: Some(Dolt::Differs(r#"dolt_backups is a view over the database's backups, so writes fail as Postgres refuses them on a view, where Dolt's table refuses them with its own message"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_backups SET name = 'backup1' WHERE name = 'backup2';"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot update view "dolt_backups""#, detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_backups VALUES ('backup4', 'file:///tmp/broken', '{}');"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot insert into view "dolt_backups""#, detail: "Views that do not select from a single table or view are not automatically updatable.", hint: "To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'backup4', 'aws://[ddb_table_4:ddb_s3_bucket_4]/db1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('remove', 'backup1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_backups WHERE url LIKE 'aws://%' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("url", TEXT), Column("params", JSONB)],
                        rows: &[
                            &[T(r#"backup2"#), T(r#"aws://[ddb_table:ddb_s3_bucket]/db1"#), T(r#"{}"#)],
                            &[T(r#"backup4"#), T(r#"aws://[ddb_table_4:ddb_s3_bucket_4]/db1"#), T(r#"{}"#)],
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
