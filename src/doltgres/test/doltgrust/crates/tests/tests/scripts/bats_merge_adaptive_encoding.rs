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

//! Dolt's integration-tests/bats/merge-adaptive-encoding.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_merge_adaptive_encoding() {
    run_scripts(&[
        ScriptTest {
            name: r#"merge-adaptive-encoding: TEXT, dest converts to adaptive (case 1)"#,
            intent: r#"A merge combines both sides' row changes when one side dropped and re-created the table with the same definition."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (1, 'aaa', 10), (2, 'bbb', 20), (4, 'ddd', 40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 11 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (3, 'ccc', 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = 'DDD' WHERE pk = 4;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c, val FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", TEXT), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"aaa"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"bbb"#), T(r#"21"#)],
                            &[T(r#"3"#), T(r#"ccc"#), T(r#"30"#)],
                            &[T(r#"4"#), T(r#"DDD"#), T(r#"40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: TEXT, source converts to adaptive (case 2)"#,
            intent: r#"A merge combines both sides' row changes when one side dropped and re-created the table with the same definition."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (1, 'aaa', 10), (2, 'bbb', 20), (4, 'ddd', 40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 11 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (3, 'ccc', 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = 'DDD' WHERE pk = 4;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c, val FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", TEXT), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"aaa"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"bbb"#), T(r#"21"#)],
                            &[T(r#"3"#), T(r#"ccc"#), T(r#"30"#)],
                            &[T(r#"4"#), T(r#"DDD"#), T(r#"40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: JSON, dest converts to adaptive (case 1)"#,
            intent: r#"A merge combines both sides' row changes when one side dropped and re-created the table with the same definition."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (1, jsonb_build_object('v', 1), 10), (2, jsonb_build_object('v', 2), 20), (4, jsonb_build_object('v', 4), 40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 11 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (3, jsonb_build_object('v', 3), 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = jsonb_build_object('v', 44) WHERE pk = 4;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c->>'v' AS v, val FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", TEXT), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"2"#), T(r#"21"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"30"#)],
                            &[T(r#"4"#), T(r#"44"#), T(r#"40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: JSON, source converts to adaptive (case 2)"#,
            intent: r#"A merge combines both sides' row changes when one side dropped and re-created the table with the same definition."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (1, jsonb_build_object('v', 1), 10), (2, jsonb_build_object('v', 2), 20), (4, jsonb_build_object('v', 4), 40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 11 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (3, jsonb_build_object('v', 3), 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = jsonb_build_object('v', 44) WHERE pk = 4;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c->>'v' AS v, val FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", TEXT), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"2"#), T(r#"21"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"30"#)],
                            &[T(r#"4"#), T(r#"44"#), T(r#"40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: TEXT with check constraint, dest converts (case 1)"#,
            intent: r#"A merge combines both sides' row changes, with no violations of the check, when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (1, 'aaa', 10), (2, 'bbb', 20), (4, 'ddd', 40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 11 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (3, 'ccc', 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = 'DDD' WHERE pk = 4;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c, val FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", TEXT), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"aaa"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"bbb"#), T(r#"21"#)],
                            &[T(r#"3"#), T(r#"ccc"#), T(r#"30"#)],
                            &[T(r#"4"#), T(r#"DDD"#), T(r#"40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: TEXT with check constraint, source converts (case 2)"#,
            intent: r#"A merge combines both sides' row changes, with no violations of the check, when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (1, 'aaa', 10), (2, 'bbb', 20), (4, 'ddd', 40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 11 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (3, 'ccc', 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = 'DDD' WHERE pk = 4;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c, val FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", TEXT), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"aaa"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"bbb"#), T(r#"21"#)],
                            &[T(r#"3"#), T(r#"ccc"#), T(r#"30"#)],
                            &[T(r#"4"#), T(r#"DDD"#), T(r#"40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: JSON with check constraint, dest converts (case 1)"#,
            intent: r#"A merge combines both sides' row changes, with no violations of the check, when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (1, jsonb_build_object('v', 1), 10), (2, jsonb_build_object('v', 2), 20), (4, jsonb_build_object('v', 4), 40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 11 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (3, jsonb_build_object('v', 3), 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = jsonb_build_object('v', 44) WHERE pk = 4;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c->>'v' AS v, val FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", TEXT), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"2"#), T(r#"21"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"30"#)],
                            &[T(r#"4"#), T(r#"44"#), T(r#"40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: JSON with check constraint, source converts (case 2)"#,
            intent: r#"A merge combines both sides' row changes, with no violations of the check, when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (1, jsonb_build_object('v', 1), 10), (2, jsonb_build_object('v', 2), 20), (4, jsonb_build_object('v', 4), 40);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 11 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) VALUES (3, jsonb_build_object('v', 3), 30);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET val = 21 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = jsonb_build_object('v', 44) WHERE pk = 4;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest changes');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, c->>'v' AS v, val FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", TEXT), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"2"#), T(r#"21"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"30"#)],
                            &[T(r#"4"#), T(r#"44"#), T(r#"40"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: structural TEXT, dest converts to adaptive (case 1)"#,
            intent: r#"A merge of a table spanning many leaves combines both sides' edits, deletes, and inserted ranges when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('BASE-', n), 1200, '.'), n FROM generate_series(2048, 4095) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base (shared range)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = rpad(concat('SMOD-', pk), 1200, '.'), val = val + 100000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 0;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 2;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('SRC-', n), 1200, '.'), n FROM generate_series(0, 2047) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source: shared edits + exclusive inserts [0,2048)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = rpad(concat('DMOD-', pk), 1200, '.'), val = val + 200000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 1;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 3;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('DST-', n), 1200, '.'), n FROM generate_series(4096, 6143) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest: shared edits + exclusive inserts [4096,6144)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5120"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'SRC-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'DST-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'SMOD-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'DMOD-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'BASE-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE pk >= 2048 AND pk < 4096 AND (pk % 4 = 2 OR pk % 4 = 3);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: structural TEXT, source converts to adaptive (case 2)"#,
            intent: r#"A merge of a table spanning many leaves combines both sides' edits, deletes, and inserted ranges when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('BASE-', n), 1200, '.'), n FROM generate_series(2048, 4095) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base (shared range)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = rpad(concat('SMOD-', pk), 1200, '.'), val = val + 100000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 0;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 2;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('SRC-', n), 1200, '.'), n FROM generate_series(0, 2047) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source: shared edits + exclusive inserts [0,2048)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = rpad(concat('DMOD-', pk), 1200, '.'), val = val + 200000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 1;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 3;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('DST-', n), 1200, '.'), n FROM generate_series(4096, 6143) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest: shared edits + exclusive inserts [4096,6144)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5120"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'SRC-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'DST-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'SMOD-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'DMOD-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'BASE-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE pk >= 2048 AND pk < 4096 AND (pk % 4 = 2 OR pk % 4 = 3);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: structural JSON, dest converts to adaptive (case 1)"#,
            intent: r#"A merge of a table spanning many leaves combines both sides' edits, deletes, and inserted ranges when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, jsonb_build_object('tag', 'BASE', 'pk', n, 'pad', repeat('x', 1190)), n FROM generate_series(2048, 4095) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base (shared range)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = jsonb_build_object('tag', 'SMOD', 'pk', pk, 'pad', repeat('x', 1190)), val = val + 100000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 0;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 2;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, jsonb_build_object('tag', 'SRC', 'pk', n, 'pad', repeat('x', 1190)), n FROM generate_series(0, 2047) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source: shared edits + exclusive inserts [0,2048)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = jsonb_build_object('tag', 'DMOD', 'pk', pk, 'pad', repeat('x', 1190)), val = val + 200000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 1;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 3;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, jsonb_build_object('tag', 'DST', 'pk', n, 'pad', repeat('x', 1190)), n FROM generate_series(4096, 6143) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest: shared edits + exclusive inserts [4096,6144)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5120"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'SRC';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'DST';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'SMOD';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'DMOD';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'BASE';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE pk >= 2048 AND pk < 4096 AND (pk % 4 = 2 OR pk % 4 = 3);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: structural JSON, source converts to adaptive (case 2)"#,
            intent: r#"A merge of a table spanning many leaves combines both sides' edits, deletes, and inserted ranges when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, jsonb_build_object('tag', 'BASE', 'pk', n, 'pad', repeat('x', 1190)), n FROM generate_series(2048, 4095) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base (shared range)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c JSONB, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = jsonb_build_object('tag', 'SMOD', 'pk', pk, 'pad', repeat('x', 1190)), val = val + 100000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 0;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 2;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, jsonb_build_object('tag', 'SRC', 'pk', n, 'pad', repeat('x', 1190)), n FROM generate_series(0, 2047) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source: shared edits + exclusive inserts [0,2048)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = jsonb_build_object('tag', 'DMOD', 'pk', pk, 'pad', repeat('x', 1190)), val = val + 200000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 1;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 3;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, jsonb_build_object('tag', 'DST', 'pk', n, 'pad', repeat('x', 1190)), n FROM generate_series(4096, 6143) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest: shared edits + exclusive inserts [4096,6144)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5120"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'SRC';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'DST';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'SMOD';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'DMOD';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c->>'tag' = 'BASE';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE pk >= 2048 AND pk < 4096 AND (pk % 4 = 2 OR pk % 4 = 3);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: structural TEXT with check constraint, dest converts (case 1)"#,
            intent: r#"A merge of a table spanning many leaves combines both sides' edits, deletes, and inserted ranges when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('BASE-', n), 1200, '.'), n FROM generate_series(2048, 4095) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base (shared range)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = rpad(concat('SMOD-', pk), 1200, '.'), val = val + 100000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 0;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 2;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('SRC-', n), 1200, '.'), n FROM generate_series(0, 2047) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source: shared edits + exclusive inserts [0,2048)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = rpad(concat('DMOD-', pk), 1200, '.'), val = val + 200000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 1;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 3;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('DST-', n), 1200, '.'), n FROM generate_series(4096, 6143) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest: shared edits + exclusive inserts [4096,6144)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5120"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'SRC-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'DST-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'SMOD-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'DMOD-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'BASE-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE pk >= 2048 AND pk < 4096 AND (pk % 4 = 2 OR pk % 4 = 3);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"merge-adaptive-encoding: structural TEXT with check constraint, source converts (case 2)"#,
            intent: r#"A merge of a table spanning many leaves combines both sides' edits, deletes, and inserted ranges when one side dropped and re-created the table."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('BASE-', n), 1200, '.'), n FROM generate_series(2048, 4095) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'base (shared range)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"DOLT_USE_ADAPTIVE_ENCODING picks Dolt's encoding of long values for each CLI process, and Doltgrust has one encoding of long values, so the port keeps the drop and re-create on that side"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, c TEXT, val INT, CHECK (val >= 0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t SELECT * FROM t AS OF 'HEAD';"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = rpad(concat('SMOD-', pk), 1200, '.'), val = val + 100000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 0;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 2;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('SRC-', n), 1200, '.'), n FROM generate_series(0, 2047) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'source: shared edits + exclusive inserts [0,2048)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t SET c = rpad(concat('DMOD-', pk), 1200, '.'), val = val + 200000 WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 1;"#,
                    expected: Expected::Tag("UPDATE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE pk >= 2048 AND pk < 4096 AND pk % 4 = 3;"#,
                    expected: Expected::Tag("DELETE 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t (pk, c, val) SELECT n, rpad(concat('DST-', n), 1200, '.'), n FROM generate_series(4096, 6143) AS s(n);"#,
                    expected: Expected::Tag("INSERT 0 2048"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dest: shared edits + exclusive inserts [4096,6144)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5120"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'SRC-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'DST-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"2048"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'SMOD-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'DMOD-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"512"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE c LIKE 'BASE-%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE pk >= 2048 AND pk < 4096 AND (pk % 4 = 2 OR pk % 4 = 3);"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
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
