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

//! The `constraints_triggers_replay` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_constraints_triggers_replay() {
    run_scripts(&[
        ScriptTest {
            name: r#"check_cherrypick_valid"#,
            intent: r#"Doltlite oracle case: check cherrypick valid."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int CHECK(v>0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO t VALUES(2,20);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_add');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
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
                    query: r#"SELECT CONCAT('R|',id,'|',v) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|10"#)],
                            &[T(r#"R|2|20"#)],
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
            name: r#"check_cherrypick_violation"#,
            intent: r#"Doltlite oracle case: check cherrypick violation."#,
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
                    query: r#"INSERT INTO t VALUES(1,10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO t VALUES(2,-5);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_bad');"#,
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
                    query: r#"CREATE TABLE t2(id integer PRIMARY KEY, v int CHECK(v>0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 SELECT * FROM t;"#,
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: r#"ALTER TABLE t2 RENAME TO t;"#,
                    expected: Expected::Tag("ALTER TABLE"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_check');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"merge aborted: schema conflict found for table t 
 please resolve schema conflicts before merging: 
	table was modified in one branch and deleted in the other"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|',id,'|',v) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|10"#)],
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
            name: r#"unique_cherrypick_valid"#,
            intent: r#"Doltlite oracle case: unique cherrypick valid."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, u int UNIQUE);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO t VALUES(2,20);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_add');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
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
                    query: r#"SELECT CONCAT('R|',id,'|',u) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|10"#)],
                            &[T(r#"R|2|20"#)],
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
            name: r#"unique_cherrypick_violation"#,
            intent: r#"Doltlite oracle case: unique cherrypick violation."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, u int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO t VALUES(2,10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_dup');"#,
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
                    query: r#"CREATE TABLE t2(id integer PRIMARY KEY, u int UNIQUE);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 SELECT * FROM t;"#,
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: r#"ALTER TABLE t2 RENAME TO t;"#,
                    expected: Expected::Tag("ALTER TABLE"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_unique');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"merge aborted: schema conflict found for table t 
 please resolve schema conflicts before merging: 
	table was modified in one branch and deleted in the other"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|',id,'|',u) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|10"#)],
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
            name: r#"fk_cherrypick_valid"#,
            intent: r#"Doltlite oracle case: fk cherrypick valid."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE p(id integer PRIMARY KEY, u int UNIQUE);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE c(id integer PRIMARY KEY, pid int, FOREIGN KEY(pid) REFERENCES p(id));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO p VALUES(1,100),(2,200);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c VALUES(1,1);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO c VALUES(2,2);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_child');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
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
                    query: r#"SELECT CONCAT('R|',id,'|',pid) FROM c ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|1"#)],
                            &[T(r#"R|2|2"#)],
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
            name: r#"fk_cherrypick_orphan"#,
            intent: r#"Doltlite oracle case: fk cherrypick orphan."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE p(id integer PRIMARY KEY, u int UNIQUE);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE c(id integer PRIMARY KEY, u int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO p VALUES(1,100);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c VALUES(1,100);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO c VALUES(2,200);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_orphan');"#,
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
                    query: r#"CREATE TABLE c2(id integer PRIMARY KEY, u int, FOREIGN KEY(u) REFERENCES p(u));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c2 SELECT * FROM c;"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE c;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c2 RENAME TO c;"#,
                    expected: Expected::Tag("ALTER TABLE"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_fk');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"merge aborted: schema conflict found for table c 
 please resolve schema conflicts before merging: 
	table was modified in one branch and deleted in the other"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|',id,'|',u) FROM c ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|100"#)],
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
            name: r#"check_rebase_valid"#,
            intent: r#"Doltlite oracle case: check rebase valid."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v int CHECK(v>0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO t VALUES(2,20);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_add');"#,
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
                    query: r#"INSERT INTO t VALUES(3,30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_advance');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feat');"#,
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
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/feat"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|',id,'|',v) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|10"#)],
                            &[T(r#"R|2|20"#)],
                            &[T(r#"R|3|30"#)],
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
            name: r#"check_rebase_violation"#,
            intent: r#"Doltlite oracle case: check rebase violation."#,
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
                    query: r#"INSERT INTO t VALUES(1,10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO t VALUES(2,-1);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_bad');"#,
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
                    query: r#"CREATE TABLE t2(id integer PRIMARY KEY, v int CHECK(v>0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 SELECT * FROM t;"#,
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: r#"ALTER TABLE t2 RENAME TO t;"#,
                    expected: Expected::Tag("ALTER TABLE"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_check');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feat');"#,
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
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"1"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::Differs(r#"merges record a primary key difference as a schema conflict, so the rebase stops there and aborts itself with Dolt's rebase message rather than failing (Appendix C)"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Already on branch 'feat'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::Differs(r#"the aborted rebase leaves the session on its branch, where Dolt's failed one leaves it off it (Appendix C)"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|',id,'|',v) FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|10"#)],
                            &[T(r#"R|2|-1"#)],
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
            name: r#"fk_rebase_valid"#,
            intent: r#"Doltlite oracle case: fk rebase valid."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE p(id integer PRIMARY KEY, u int UNIQUE);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE c(id integer PRIMARY KEY, pid int, FOREIGN KEY(pid) REFERENCES p(id));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO p VALUES(1,100);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c VALUES(1,1);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO p VALUES(2,200);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c VALUES(2,2);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_add');"#,
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
                    query: r#"INSERT INTO p VALUES(3,300);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_advance');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feat');"#,
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
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/feat"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|',id,'|',pid) FROM c ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|1"#)],
                            &[T(r#"R|2|2"#)],
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
            name: r#"fk_rebase_orphan"#,
            intent: r#"Doltlite oracle case: fk rebase orphan."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE p(id integer PRIMARY KEY, u int UNIQUE);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE c(id integer PRIMARY KEY, u int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO p VALUES(1,100);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c VALUES(1,100);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','init');"#,
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
                    query: r#"INSERT INTO c VALUES(2,200);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_orphan');"#,
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
                    query: r#"CREATE TABLE c2(id integer PRIMARY KEY, u int, FOREIGN KEY(u) REFERENCES p(u));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c2 SELECT * FROM c;"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE c;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c2 RENAME TO c;"#,
                    expected: Expected::Tag("ALTER TABLE"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_fk');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feat');"#,
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
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"1"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::Differs(r#"merges record a primary key difference as a schema conflict, so the rebase stops there and aborts itself with Dolt's rebase message rather than failing (Appendix C)"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Already on branch 'feat'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::Differs(r#"the aborted rebase leaves the session on its branch, where Dolt's failed one leaves it off it (Appendix C)"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|',id,'|',u) FROM c ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|100"#)],
                            &[T(r#"R|2|200"#)],
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
            name: r#"trigger_insert_log_travels_with_cherrypick"#,
            intent: r#"Doltlite oracle case: trigger insert log travels with cherrypick."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v VARCHAR(16));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE log(id integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION "t_ai_oracle_trigger"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN INSERT INTO log VALUES(new.id) ON CONFLICT DO NOTHING; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER "t_ai" AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION "t_ai_oracle_trigger"();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','base');"#,
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
                    query: r#"INSERT INTO t VALUES(2,'b');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(3,'c');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_inserts');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
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
                    query: r#"SELECT CONCAT('R|',id) FROM log ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1"#)],
                            &[T(r#"R|2"#)],
                            &[T(r#"R|3"#)],
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
            name: r#"trigger_update_log_travels_with_cherrypick"#,
            intent: r#"Doltlite oracle case: trigger update log travels with cherrypick."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE log(id integer PRIMARY KEY, delta integer);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION "t_au_oracle_trigger"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN INSERT INTO log VALUES(new.id, new.v - old.v); IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER "t_au" AFTER UPDATE ON t FOR EACH ROW EXECUTE FUNCTION "t_au_oracle_trigger"();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,10),(2,20);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','base');"#,
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
                    query: r#"UPDATE t SET v=30 WHERE id=1;"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_update');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
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
                    query: r#"SELECT CONCAT('R|',id,'|',coalesce((delta)::text, ('')::text)) FROM log ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|20"#)],
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
            name: r#"trigger_delete_log_travels_with_cherrypick"#,
            intent: r#"Doltlite oracle case: trigger delete log travels with cherrypick."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v VARCHAR(8));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE log(id integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION "t_ad_oracle_trigger"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN INSERT INTO log VALUES(old.id) ON CONFLICT DO NOTHING; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER "t_ad" AFTER DELETE ON t FOR EACH ROW EXECUTE FUNCTION "t_ad_oracle_trigger"();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','base');"#,
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
                    query: r#"DELETE FROM t WHERE id>=2;"#,
                    expected: Expected::Tag("DELETE 2"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_delete');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
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
                    query: r#"SELECT CONCAT('R|',id) FROM log ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|2"#)],
                            &[T(r#"R|3"#)],
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
            name: r#"trigger_preserved_after_rebase_fires_on_insert"#,
            intent: r#"Doltlite oracle case: trigger preserved after rebase fires on insert."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v VARCHAR(16));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE log(id integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','base');"#,
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
                    query: r#"CREATE OR REPLACE FUNCTION "t_ai_oracle_trigger"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN INSERT INTO log VALUES(new.id) ON CONFLICT DO NOTHING; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER "t_ai" AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION "t_ai_oracle_trigger"();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_trigger');"#,
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
                    query: r#"INSERT INTO t VALUES(2,'b');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_data');"#,
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
                    query: r#"INSERT INTO t VALUES(10,'main');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_advance');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feat');"#,
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
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/feat"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(20,'after_rebase');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','post_rebase');"#,
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
                    query: r#"SELECT CONCAT('R|',id) FROM log ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|2"#)],
                            &[T(r#"R|20"#)],
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
            name: r#"trigger_rebase_multi_commit_post_insert"#,
            intent: r#"Doltlite oracle case: trigger rebase multi commit post insert."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, v VARCHAR(8));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE log(id integer PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'a');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','base');"#,
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
                    query: r#"CREATE OR REPLACE FUNCTION "t_ai_oracle_trigger"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN INSERT INTO log VALUES(new.id) ON CONFLICT DO NOTHING; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER "t_ai" AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION "t_ai_oracle_trigger"();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_trigger');"#,
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
                    query: r#"INSERT INTO t VALUES(2,'b');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_c2');"#,
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
                    query: r#"INSERT INTO t VALUES(3,'c');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_c3');"#,
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
                    query: r#"INSERT INTO t VALUES(10,'m');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','main_advance');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feat');"#,
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
                    query: r#"SELECT * FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/feat"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(30,'post');"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','post_rebase');"#,
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
                    query: r#"SELECT CONCAT('R|',id) FROM log ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|2"#)],
                            &[T(r#"R|3"#)],
                            &[T(r#"R|30"#)],
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
            name: r#"trigger_composite_pk_cherrypick_fires_on_target_insert"#,
            intent: r#"Doltlite oracle case: trigger composite pk cherrypick fires on target insert."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(a integer, b VARCHAR(4), v integer, PRIMARY KEY(a, b));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE log(a integer, b VARCHAR(4), PRIMARY KEY(a, b));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE OR REPLACE FUNCTION "t_ai_oracle_trigger"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN INSERT INTO log VALUES(new.a, new.b) ON CONFLICT DO NOTHING; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER "t_ai" AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION "t_ai_oracle_trigger"();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'x',10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','base');"#,
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
                    query: r#"INSERT INTO t VALUES(2,'y',20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'z',30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_inserts');"#,
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
                    query: r#"SELECT * FROM dolt_cherry_pick('feat');"#,
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
                    query: r#"SELECT CONCAT('R|',a,'|',b) FROM log ORDER BY a,b;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|1|x"#)],
                            &[T(r#"R|2|y"#)],
                            &[T(r#"R|2|z"#)],
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
            name: r#"trigger_composite_pk_post_merge_fires"#,
            intent: r#"Doltlite oracle case: trigger composite pk post merge fires."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(a integer, b VARCHAR(4), v integer, PRIMARY KEY(a, b));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE log(a integer, b VARCHAR(4), PRIMARY KEY(a, b));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,'x',10);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','base');"#,
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
                    query: r#"CREATE OR REPLACE FUNCTION "t_ai_oracle_trigger"() RETURNS trigger LANGUAGE plpgsql AS $oracle$ BEGIN INSERT INTO log VALUES(new.a, new.b) ON CONFLICT DO NOTHING; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $oracle$;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TRIGGER "t_ai" AFTER INSERT ON t FOR EACH ROW EXECUTE FUNCTION "t_ai_oracle_trigger"();"#,
                    expected: Expected::Tag("CREATE TRIGGER"),
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
                    query: r#"SELECT * FROM dolt_commit('-m','feat_trigger');"#,
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
                    query: r#"SELECT * FROM dolt_merge('feat');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"hash"#, TEXT), Column(r#"fast_forward"#, INT8), Column(r#"conflicts"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'y',20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(2,'z',30);"#,
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
                    query: r#"SELECT * FROM dolt_commit('-m','post_merge');"#,
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
                    query: r#"SELECT CONCAT('R|',a,'|',b) FROM log ORDER BY a,b;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|2|y"#)],
                            &[T(r#"R|2|z"#)],
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
            name: r#"quoted_check_identifiers"#,
            intent: r#"Doltlite oracle case: quoted check identifiers."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, "CHECK(0)" int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','base');"#,
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
                    query: r#"INSERT INTO t VALUES(2,2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: r#"INSERT INTO t VALUES(3,3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: r#"SELECT * FROM dolt_verify_constraints('--all');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"violations"#, INT8)],
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
                    query: r#"SELECT CONCAT('R|rows=', COUNT(*)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|rows=3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|violations=', COUNT(*)) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|violations=0"#)],
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
            name: r#"quoted_check_expression"#,
            intent: r#"Doltlite oracle case: quoted check expression."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY, "a)b" int CHECK("a)b">0));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1,1),(2,NULL);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-Am','base');"#,
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
                    query: r#"INSERT INTO t VALUES(3,2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: r#"INSERT INTO t VALUES(4,3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: r#"SELECT * FROM dolt_verify_constraints('--all');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"violations"#, INT8)],
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
                    query: r#"SELECT CONCAT('R|rows=', COUNT(*)) FROM t;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|rows=4"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|violations=', COUNT(*)) FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|violations=0"#)],
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
