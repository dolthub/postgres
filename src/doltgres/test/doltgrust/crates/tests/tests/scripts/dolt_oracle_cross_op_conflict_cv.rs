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

//! The `cross_op_conflict_cv` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_cross_op_conflict_cv() {
    run_scripts(&[
        ScriptTest {
            name: r#"merge_conflict_and_cv"#,
            intent: r#"Doltlite oracle case: merge conflict and cv."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Tag("SET"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, u int UNIQUE, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,1,'base'),(2,2,'base');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','init');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b','feat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feat'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='feat' WHERE id=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET u=9 WHERE id=2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','feat');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='main', u=9 WHERE id=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','main');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('feat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"fast_forward"#, INT8), Column(r#"conflicts"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..E }],
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('S|',
  (SELECT COUNT(*) FROM dolt_conflicts), '|',
  (SELECT COUNT(*) FROM dolt_constraint_violations), '|',
  (SELECT string_agg((CONCAT(id, ':', u, ':', v))::text, ',' ORDER BY id) FROM t));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S|1|1|1:9:main,2:9:base"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"cherry_pick_conflict_and_cv"#,
            intent: r#"Doltlite oracle case: cherry pick conflict and cv."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Tag("SET"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, u int UNIQUE, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,1,'base'),(2,2,'base');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','init');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b','feat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feat'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='feat' WHERE id=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET u=9 WHERE id=2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','feat');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='main', u=9 WHERE id=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','main');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"data_conflicts"#, INT8), Column(r#"schema_conflicts"#, INT8), Column(r#"constraint_violations"#, INT8)],
                        rows: &[
                            &[T(r#""#), T(r#"1"#), T(r#"0"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('S|',
  (SELECT COUNT(*) FROM dolt_conflicts), '|',
  (SELECT COUNT(*) FROM dolt_constraint_violations), '|',
  (SELECT string_agg((CONCAT(id, ':', u, ':', v))::text, ',' ORDER BY id) FROM t));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S|1|1|1:9:main,2:9:base"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"revert_conflict_and_cv"#,
            intent: r#"Doltlite oracle case: revert conflict and cv."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Tag("SET"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, u int UNIQUE, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1,1,'base'),(2,2,'base');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','init');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET u=9, v='mid' WHERE id=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','c_mid');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET u=1, v='end2' WHERE id=2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v='end1' WHERE id=1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','c_end');"#,
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
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_revert('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"data_conflicts"#, INT8), Column(r#"schema_conflicts"#, INT8), Column(r#"constraint_violations"#, INT8)],
                        rows: &[
                            &[T(r#""#), T(r#"1"#), T(r#"0"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('S|',
  (SELECT COUNT(*) FROM dolt_conflicts), '|',
  (SELECT COUNT(*) FROM dolt_constraint_violations), '|',
  (SELECT string_agg((CONCAT(id, ':', u, ':', v))::text, ',' ORDER BY id) FROM t));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S|1|0|1:9:end1,2:1:end2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RELEASE SAVEPOINT oracle_statement;"#,
                    expected: Expected::Tag("RELEASE"),
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
            ],
            ..S
        },
    ]);
}
