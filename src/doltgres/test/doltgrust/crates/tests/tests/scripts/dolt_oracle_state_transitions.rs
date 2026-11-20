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

//! The `state_transitions` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_state_transitions() {
    run_scripts(&[
        ScriptTest {
            name: r#"reset_unstages_while_working_has_separate_diff"#,
            intent: r#"Doltlite oracle case: reset unstages while working has separate diff."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
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
                    query: r#"INSERT INTO t VALUES (4, 40);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (4, 40);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.t	f	modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (4, 40);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	t	3	30"#)],
                            &[T(r#"T	t	4	40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"hard_reset_undoes_stage_then_working_delete"#,
            intent: r#"Doltlite oracle case: hard reset undoes stage then working delete."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
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
                    query: r#"DELETE FROM t WHERE id = 3;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE id = 3;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE id = 3;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"mixed_reset_preserves_subsequent_working_change"#,
            intent: r#"Doltlite oracle case: mixed reset preserves subsequent working change."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
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
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.t	f	modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	200"#)],
                            &[T(r#"T	t	2	20"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"hard_reset_wipes_both_staged_and_working"#,
            intent: r#"Doltlite oracle case: hard reset wipes both staged and working."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
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
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"table_reset_unstages_only_named_table"#,
            intent: r#"Doltlite oracle case: table reset unstages only named table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE b(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (1, 100);"#,
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
                    query: r#"INSERT INTO a VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (2, 200);"#,
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
                    query: r#"SELECT * FROM dolt_reset('a');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE b(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (1, 100);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (2, 200);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('a');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.b	t	modified"#)],
                            &[T(r#"S	public.a	f	modified"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE b(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (1, 100);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (2, 200);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('a');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 'a', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM a UNION ALL SELECT concat('T', chr(9), 'b', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM b;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	a	1	10"#)],
                            &[T(r#"T	a	2	20"#)],
                            &[T(r#"T	b	1	100"#)],
                            &[T(r#"T	b	2	200"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"mixed_reset_to_prev_commit_moves_diff_to_working"#,
            intent: r#"Doltlite oracle case: mixed reset to prev commit moves diff to working."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
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
                    query: r#"SELECT * FROM dolt_reset('HEAD~1');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.t	f	modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	t	3	30"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"hard_reset_to_prev_commit_drops_diff_entirely"#,
            intent: r#"Doltlite oracle case: hard reset to prev commit drops diff entirely."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
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
                    query: r#"SELECT * FROM dolt_reset('--hard', 'HEAD~1');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard', 'HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard', 'HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"checkout_branch_clean_swaps_working"#,
            intent: r#"Doltlite oracle case: checkout branch clean swaps working."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'feat1');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'feat1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'feat1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"checkout_branch_per_branch_working_set"#,
            intent: r#"Doltlite oracle case: checkout branch per branch working set."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
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
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.t	f	modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	t	3	30"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"checkout_b_starts_from_head_not_working"#,
            intent: r#"Doltlite oracle case: checkout b starts from head not working."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
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
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'feature2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature2'"#)],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'feature2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature2'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.t	f	modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'feature2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature2'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	t	3	30"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"checkout_table_reverts_only_named_table"#,
            intent: r#"Doltlite oracle case: checkout table reverts only named table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE b(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (1, 100);"#,
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
                    query: r#"UPDATE a SET v = 999 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE b SET v = 999 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('a');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#""#)],
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
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE b(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (1, 100);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE a SET v = 999 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE b SET v = 999 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('a');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#""#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.b	f	modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE b(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO b VALUES (1, 100);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE a SET v = 999 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE b SET v = 999 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('a');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#""#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 'a', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM a UNION ALL SELECT concat('T', chr(9), 'b', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM b;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	a	1	10"#)],
                            &[T(r#"T	b	1	999"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"checkout_table_clears_both_staged_and_working"#,
            intent: r#"Doltlite oracle case: checkout table clears both staged and working."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
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
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#""#)],
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
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#""#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.t	t	modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 100 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET v = 200 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('t');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#""#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	100"#)],
                            &[T(r#"T	t	2	20"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"soft_reset_uncommit_then_recommit"#,
            intent: r#"Doltlite oracle case: soft reset uncommit then recommit."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
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
                    query: r#"SELECT * FROM dolt_reset('--soft', 'HEAD~1');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2-recommitted');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--soft', 'HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2-recommitted');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--soft', 'HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2-recommitted');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	t	3	30"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"mixed_reset_uncommit_then_readd_recommit"#,
            intent: r#"Doltlite oracle case: mixed reset uncommit then readd recommit."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
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
                    query: r#"SELECT * FROM dolt_reset('HEAD~1');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2-take-two');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2-take-two');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'c2-take-two');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	t	3	30"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"branch_round_trip_preserves_each_side"#,
            intent: r#"Doltlite oracle case: branch round trip preserves each side."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'feat1');"#,
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
                    query: r#"INSERT INTO t VALUES (4, 40);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'main2');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'feat1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (4, 40);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'main2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'feat1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (4, 40);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'main2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	t	4	40"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"reset_undoes_staged_table_deletion"#,
            intent: r#"Doltlite oracle case: reset undoes staged table deletion."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
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
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"mixed_reset_does_not_touch_untracked_new_table"#,
            intent: r#"Doltlite oracle case: mixed reset does not touch untracked new table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.u	f	new table"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset();"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t UNION ALL SELECT concat('T', chr(9), 'u', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM u;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	u	1	99"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"hard_reset_with_untracked_new_table"#,
            intent: r#"Doltlite oracle case: hard reset with untracked new table."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"S	public.u	f	new table"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t UNION ALL SELECT concat('T', chr(9), 'u', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM u;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                            &[T(r#"T	u	1	99"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"hard_reset_drops_table_added_after_target"#,
            intent: r#"Doltlite oracle case: hard reset drops table added after target."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
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
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'add_u');"#,
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
                    query: r#"SELECT * FROM dolt_reset('--hard', 'HEAD~1');"#,
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
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('L', chr(9), commit_hash, chr(9), message) FROM dolt_log ORDER BY commit_order DESC;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
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
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db1";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s5",
                    database: r#"db1"#,
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
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'add_u');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard', 'HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s5",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('S', chr(9), table_name, chr(9), staged, chr(9), status) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s6",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s6",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db2";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE u(id integer PRIMARY KEY, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO u VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s8",
                    database: r#"db2"#,
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
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-m', 'add_u');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_reset('--hard', 'HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT concat('T', chr(9), 't', chr(9), coalesce((id)::text, ('')::text), chr(9), coalesce((v)::text, ('')::text)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"T	t	1	10"#)],
                            &[T(r#"T	t	2	20"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s9",
                    database: r#"db2"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
    ]);
}
