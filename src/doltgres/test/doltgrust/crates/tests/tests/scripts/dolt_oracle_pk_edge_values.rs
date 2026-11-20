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

//! The `pk_edge_values` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_pk_edge_values() {
    run_scripts(&[
        ScriptTest {
            name: r#"int64_diff"#,
            intent: r#"Doltlite oracle case: int64 diff."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k bigint PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (-9223372036854775808,'min'), (-1,'neg'), (0,'zero'),
                     (1,'one'), (9223372036854775807,'max');"#,
                    expected: Expected::Tag("INSERT 0 5"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = -1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (-9223372036854775807,'min1');"#,
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
                    query: r#"SELECT CONCAT('R|', coalesce((to_k)::text, ('')::text), '|', coalesce((from_k)::text, ('')::text), '|', dolt_diff_type) FROM dolt_diff('HEAD~1','HEAD', 't');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|-9223372036854775807||added"#)],
                            &[T(r#"R||-1|removed"#)],
                        ],
                        tag: "SELECT 2",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"int64_history"#,
            intent: r#"Doltlite oracle case: int64 history."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k bigint PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (-9223372036854775808,'min'), (-1,'neg'), (0,'zero'),
                     (1,'one'), (9223372036854775807,'max');"#,
                    expected: Expected::Tag("INSERT 0 5"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = -1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (-9223372036854775807,'min1');"#,
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
                    query: r#"SELECT CONCAT('R|', k, '=', v) FROM dolt_history_t WHERE dolt_commit_hash = (SELECT commit_hash FROM dolt_log LIMIT 1 OFFSET 1);"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|-9223372036854775808=min"#)],
                            &[T(r#"R|-1=neg"#)],
                            &[T(r#"R|0=zero"#)],
                            &[T(r#"R|1=one"#)],
                            &[T(r#"R|9223372036854775807=max"#)],
                        ],
                        tag: "SELECT 5",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"int64_state"#,
            intent: r#"Doltlite oracle case: int64 state."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k bigint PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (-9223372036854775808,'min'), (-1,'neg'), (0,'zero'),
                     (1,'one'), (9223372036854775807,'max');"#,
                    expected: Expected::Tag("INSERT 0 5"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = -1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (-9223372036854775807,'min1');"#,
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
                    query: r#"SELECT CONCAT('R|', k, '=', v) FROM t ORDER BY k;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|-9223372036854775808=min"#)],
                            &[T(r#"R|-9223372036854775807=min1"#)],
                            &[T(r#"R|0=zero"#)],
                            &[T(r#"R|1=one"#)],
                            &[T(r#"R|9223372036854775807=max"#)],
                        ],
                        tag: "SELECT 5",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"int64_merge"#,
            intent: r#"Doltlite oracle case: int64 merge."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k bigint PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (0,'zero');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'feat');"#,
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
                    query: r#"INSERT INTO t VALUES (-9223372036854775808,'min'), (9223372036854775807,'max');"#,
                    expected: Expected::Tag("INSERT 0 2"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'feat');"#,
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
                    query: r#"INSERT INTO t VALUES (-42,'neg');"#,
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
                    query: r#"SELECT * FROM dolt_merge('feat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"fast_forward"#, INT8), Column(r#"conflicts"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', k, '=', v) FROM t ORDER BY k;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|-9223372036854775808=min"#)],
                            &[T(r#"R|-42=neg"#)],
                            &[T(r#"R|0=zero"#)],
                            &[T(r#"R|9223372036854775807=max"#)],
                        ],
                        tag: "SELECT 4",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"text_edge_diff"#,
            intent: r#"Doltlite oracle case: text edge diff."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k VARCHAR(1100) PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('','empty'), ('żółć','unicode'), ('ascii','plain');"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('long_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx', 'bigkey');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = 'żółć';"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('後で','unicode2');"#,
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
                    query: r#"SELECT CONCAT('R|', SUBSTR(upper(encode((coalesce((to_k)::text, ('')::text))::text::bytea, 'hex')),1,12), '|', OCTET_LENGTH(coalesce((to_k)::text, ('')::text)), '|', SUBSTR(upper(encode((coalesce((from_k)::text, ('')::text))::text::bytea, 'hex')),1,12), '|', dolt_diff_type) FROM dolt_diff('HEAD~1','HEAD', 't');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R||0|C5BCC3B3C582|removed"#)],
                            &[T(r#"R|E5BE8CE381A7|6||added"#)],
                        ],
                        tag: "SELECT 2",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"text_edge_state"#,
            intent: r#"Doltlite oracle case: text edge state."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k VARCHAR(1100) PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('','empty'), ('żółć','unicode'), ('ascii','plain');"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('long_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx', 'bigkey');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = 'żółć';"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('後で','unicode2');"#,
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
                    query: r#"SELECT CONCAT('R|', SUBSTR(upper(encode((k)::text::bytea, 'hex')),1,12), '|', OCTET_LENGTH(k), '=', v) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R||0=empty"#)],
                            &[T(r#"R|6173636969|5=plain"#)],
                            &[T(r#"R|6C6F6E675F78|1005=bigkey"#)],
                            &[T(r#"R|E5BE8CE381A7|6=unicode2"#)],
                        ],
                        tag: "SELECT 4",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"text_edge_history"#,
            intent: r#"Doltlite oracle case: text edge history."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k VARCHAR(1100) PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('','empty'), ('żółć','unicode'), ('ascii','plain');"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('long_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx', 'bigkey');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = 'żółć';"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('後で','unicode2');"#,
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
                    query: r#"SELECT CONCAT('R|', SUBSTR(upper(encode((k)::text::bytea, 'hex')),1,12), '|', OCTET_LENGTH(k)) FROM dolt_history_t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R||0"#)],
                            &[T(r#"R|6173636969|5"#)],
                            &[T(r#"R|6C6F6E675F78|1005"#)],
                            &[T(r#"R|E5BE8CE381A7|6"#)],
                            &[T(r#"R||0"#)],
                            &[T(r#"R|6173636969|5"#)],
                            &[T(r#"R|6C6F6E675F78|1005"#)],
                            &[T(r#"R|C5BCC3B3C582|8"#)],
                        ],
                        tag: "SELECT 8",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"binary_diff"#,
            intent: r#"Doltlite oracle case: binary diff."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k bytea PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('\x00'::bytea,'nul'), ('\xDEADBEEF'::bytea,'deadbeef'), ('\xFF00FF'::bytea,'stripes');"#,
                    expected: Expected::Tag("INSERT 0 3"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = '\xDEADBEEF'::bytea;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('\x0102030405060708090A0B0C0D0E0F10'::bytea,'sixteen');"#,
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
                    query: r#"SELECT CONCAT('R|', coalesce((upper(encode((to_k)::text::bytea, 'hex')))::text, ('')::text), '|', coalesce((upper(encode((from_k)::text::bytea, 'hex')))::text, ('')::text), '|', dolt_diff_type) FROM dolt_diff('HEAD~1','HEAD', 't');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|0102030405060708090A0B0C0D0E0F10||added"#)],
                            &[T(r#"R||DEADBEEF|removed"#)],
                        ],
                        tag: "SELECT 2",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"binary_state"#,
            intent: r#"Doltlite oracle case: binary state."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k bytea PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('\x00'::bytea,'nul'), ('\xDEADBEEF'::bytea,'deadbeef'), ('\xFF00FF'::bytea,'stripes');"#,
                    expected: Expected::Tag("INSERT 0 3"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = '\xDEADBEEF'::bytea;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('\x0102030405060708090A0B0C0D0E0F10'::bytea,'sixteen');"#,
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
                    query: r#"SELECT CONCAT('R|', upper(encode((k)::text::bytea, 'hex')), '=', v) FROM t ORDER BY k;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|00=nul"#)],
                            &[T(r#"R|0102030405060708090A0B0C0D0E0F10=sixteen"#)],
                            &[T(r#"R|FF00FF=stripes"#)],
                        ],
                        tag: "SELECT 3",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"binary_history"#,
            intent: r#"Doltlite oracle case: binary history."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k bytea PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('\x00'::bytea,'nul'), ('\xDEADBEEF'::bytea,'deadbeef'), ('\xFF00FF'::bytea,'stripes');"#,
                    expected: Expected::Tag("INSERT 0 3"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = '\xDEADBEEF'::bytea;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES ('\x0102030405060708090A0B0C0D0E0F10'::bytea,'sixteen');"#,
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
                    query: r#"SELECT CONCAT('R|', upper(encode((k)::text::bytea, 'hex'))) FROM dolt_history_t WHERE dolt_commit_hash = (SELECT commit_hash FROM dolt_log LIMIT 1 OFFSET 1);"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|00"#)],
                            &[T(r#"R|DEADBEEF"#)],
                            &[T(r#"R|FF00FF"#)],
                        ],
                        tag: "SELECT 3",
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"double_membership"#,
            intent: r#"Doltlite oracle case: double membership."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k double precision PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3.25,'a'), (-1.5,'b'), (1e15,'big'), (0.001,'small');"#,
                    expected: Expected::Tag("INSERT 0 4"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = -1.5;"#,
                    expected: Expected::Tag("DELETE 1"),
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
                    query: r#"SELECT CONCAT('R|', (SELECT count(*) FROM t), '|', (SELECT count(*) FROM t WHERE k = 3.25), '|', (SELECT count(*) FROM t WHERE k = 1e15), '|', (SELECT v FROM t WHERE k = 0.001));"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|3|1|1|small"#)],
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"double_diff_count"#,
            intent: r#"Doltlite oracle case: double diff count."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(k double precision PRIMARY KEY, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3.25,'a'), (-1.5,'b'), (1e15,'big'), (0.001,'small');"#,
                    expected: Expected::Tag("INSERT 0 4"),
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
                    query: r#"SELECT * FROM dolt_commit('-m', 'seed');"#,
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
                    query: r#"DELETE FROM t WHERE k = -1.5;"#,
                    expected: Expected::Tag("DELETE 1"),
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
                    query: r#"SELECT CONCAT('R|', dolt_diff_type, '|', count(*)) FROM dolt_diff('HEAD~1','HEAD', 't') GROUP BY dolt_diff_type;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|removed|1"#)],
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
            ],
            ..S
        },
    ]);
}
