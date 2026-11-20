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

//! The `revert` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_revert() {
    run_scripts(&[
        ScriptTest {
            name: r#"revert_head_table_creation"#,
            intent: r#"Doltlite oracle case: revert head table creation."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, x TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1, 'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'add row');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert((SELECT commit_hash FROM dolt_log LIMIT 1));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"data_conflicts"#, INT8), Column(r#"schema_conflicts"#, INT8), Column(r#"constraint_violations"#, INT8)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('L|', message) FROM dolt_log
  WHERE message IN ('add row', 'Initialize data repository')
     OR message LIKE 'Revert%';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"L|Revert "add row""#)],
                            &[T(r#"L|add row"#)],
                            &[T(r#"L|Initialize data repository"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s3",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert_non_head_clean"#,
            intent: r#"Doltlite oracle case: revert non head clean."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, x TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'schema');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1, 'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'add 1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2, 'b');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'add 2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert((SELECT commit_hash FROM dolt_log WHERE message='add 1'));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"data_conflicts"#, INT8), Column(r#"schema_conflicts"#, INT8), Column(r#"constraint_violations"#, INT8)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('T|', id, '|', x) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|2|b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('L|', message) FROM dolt_log
  WHERE message IN ('schema', 'add 1', 'add 2')
     OR message LIKE 'Revert%';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"L|Revert "add 1""#)],
                            &[T(r#"L|add 2"#)],
                            &[T(r#"L|add 1"#)],
                            &[T(r#"L|schema"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s3",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert_head_dirty_unrelated_table_committed"#,
            intent: r#"Doltlite oracle case: revert head dirty unrelated table committed."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, x TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE meta(id integer PRIMARY KEY, note TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'schema');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1, 'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'add row');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO meta VALUES(1, 'side');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert((SELECT commit_hash FROM dolt_log LIMIT 1));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"data_conflicts"#, INT8), Column(r#"schema_conflicts"#, INT8), Column(r#"constraint_violations"#, INT8)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('T|', id, '|', x) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('M|', id, '|', note) FROM meta ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"M|1|side"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('L|', message) FROM dolt_log
  WHERE message IN ('schema', 'add row') OR message LIKE 'Revert%';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"L|Revert "add row""#)],
                            &[T(r#"L|add row"#)],
                            &[T(r#"L|schema"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s3",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert_non_head_dirty_unrelated_table_committed"#,
            intent: r#"Doltlite oracle case: revert non head dirty unrelated table committed."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, x TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE meta(id integer PRIMARY KEY, note TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'schema');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1, 'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'add 1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2, 'b');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'add 2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO meta VALUES(1, 'side');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert((SELECT commit_hash FROM dolt_log WHERE message='add 1'));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"data_conflicts"#, INT8), Column(r#"schema_conflicts"#, INT8), Column(r#"constraint_violations"#, INT8)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('T|', id, '|', x) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|2|b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('M|', id, '|', note) FROM meta ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"M|1|side"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('L|', message) FROM dolt_log
  WHERE message IN ('schema', 'add 1', 'add 2') OR message LIKE 'Revert%';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"L|Revert "add 1""#)],
                            &[T(r#"L|add 2"#)],
                            &[T(r#"L|add 1"#)],
                            &[T(r#"L|schema"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s3",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert_head_dirty_same_table_rejected"#,
            intent: r#"Doltlite oracle case: revert head dirty same table rejected."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, x TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'schema');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1, 'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'add row');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2, 'side');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert((SELECT commit_hash FROM dolt_log LIMIT 1));"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: Your local changes would be overwritten by revert.
hint: Please commit your changes before you revert."#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('T|', id, '|', x) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|1|a"#)],
                            &[T(r#"T|2|side"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('S|', table_name, '|', staged, '|', status)
  FROM dolt_status ORDER BY table_name, staged, status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S|public.t|f|modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('L|', message) FROM dolt_log
  WHERE message IN ('schema', 'add row') OR message LIKE 'Revert%';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"L|add row"#)],
                            &[T(r#"L|schema"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s3",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert_head_staged_unrelated_table_committed"#,
            intent: r#"Doltlite oracle case: revert head staged unrelated table committed."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, x TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE meta(id integer PRIMARY KEY, note TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'schema');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1, 'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am', 'add row');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO meta VALUES(1, 'side');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_add('meta');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert((SELECT commit_hash FROM dolt_log LIMIT 1));"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: Your local changes would be overwritten by revert.
hint: Please commit your changes before you revert."#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('T|', id, '|', x) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T|1|a"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('M|', id, '|', note) FROM meta ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"M|1|side"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('L|', message) FROM dolt_log
  WHERE message IN ('schema', 'add row') OR message LIKE 'Revert%';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"L|add row"#)],
                            &[T(r#"L|schema"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s3",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert_author_override"#,
            intent: r#"Doltlite oracle case: revert author override."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1, 'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2, 'b');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-am', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD', '--author', 'Ann <ann@x.com>');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"data_conflicts"#, INT8), Column(r#"schema_conflicts"#, INT8), Column(r#"constraint_violations"#, INT8)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('L|', committer, '|', coalesce((email)::text, ('')::text), '|', message)
  FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"L|Ann|ann@x.com|Revert "c2""#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('C|', count(*)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"C|1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s3",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
    ]);
}
