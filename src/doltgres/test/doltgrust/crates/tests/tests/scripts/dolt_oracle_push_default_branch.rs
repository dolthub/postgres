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

//! The `push_default_branch` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_push_default_branch() {
    run_scripts(&[
        ScriptTest {
            name: r#"foo1key"#,

            intent: r#"Doltlite oracle case: foo1key."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'one');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b','foo1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'foo1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (2, 'two');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c2 on foo1');"#,
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
                    query: r#"UPDATE example SET value='ONE' WHERE id=1;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c3 on foo1');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b','bar');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'bar'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (9, 'unpushed');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c4 on bar');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('foo1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'foo1'"#)],
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_push('origin', 'foo1');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|foo1"#)],
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
                    query: r#"SELECT CONCAT('R|', name) FROM dolt_branches;"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|foo1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s7",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s7",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', id, '|', value) FROM example;"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|1|ONE"#)],
                            &[T(r#"R|2|two"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s8",
                    database: r#"db1"#,
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
                    query: r#"SELECT CONCAT('R|', message) FROM dolt_log;"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|c3 on foo1"#)],
                            &[T(r#"R|c2 on foo1"#)],
                            &[T(r#"R|c1 base"#)],
                            &[T(r#"R|Initialize data repository"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "s9",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s9",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', count(*)) FROM dolt_log;"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|4"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s10",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s10",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', id, '|', value) FROM dolt_history_example;"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|1|ONE"#)],
                            &[T(r#"R|2|two"#)],
                            &[T(r#"R|1|one"#)],
                            &[T(r#"R|2|two"#)],
                            &[T(r#"R|1|one"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "s11",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s11",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', count(*)) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s12",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s12",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"post_commit_contents"#,

            intent: r#"Doltlite oracle case: post commit contents."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (3, 'three');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "example" does not exist"#, position: 13, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c5 post-consume');"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"nothing to commit"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
                    query: r#"SELECT CONCAT('R|', id, '|', value) FROM example;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "example" does not exist"#, position: 42, ..E }),
                    client: "s3",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
            name: r#"post_commit_log"#,

            intent: r#"Doltlite oracle case: post commit log."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (3, 'three');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "example" does not exist"#, position: 13, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c5 post-consume');"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"nothing to commit"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
                    query: r#"SELECT CONCAT('R|', message) FROM dolt_log;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|Initialize data repository"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
            name: r#"post_commit_status"#,

            intent: r#"Doltlite oracle case: post commit status."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (3, 'three');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "example" does not exist"#, position: 13, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c5 post-consume');"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"nothing to commit"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
                    query: r#"SELECT CONCAT('R|', count(*)) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|0"#)],
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
        ScriptTest {
            name: r#"post_branch_active"#,

            intent: r#"Doltlite oracle case: post branch active."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b','local-work');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'local-work'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (4, 'four');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "example" does not exist"#, position: 13, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c5 on local-work');"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"nothing to commit"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
            name: r#"post_branch_list"#,

            intent: r#"Doltlite oracle case: post branch list."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b','local-work');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'local-work'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (4, 'four');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "example" does not exist"#, position: 13, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c5 on local-work');"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"nothing to commit"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
                    query: r#"SELECT CONCAT('R|', name) FROM dolt_branches;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|local-work"#)],
                            &[T(r#"R|main"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s3",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
            name: r#"post_branch_contents"#,

            intent: r#"Doltlite oracle case: post branch contents."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b','local-work');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'local-work'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (4, 'four');"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "example" does not exist"#, position: 13, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c5 on local-work');"#,
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"nothing to commit"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
                    query: r#"SELECT CONCAT('R|', id, '|', value) FROM example;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "example" does not exist"#, position: 42, ..E }),
                    client: "s3",
                    database: r#"db0"#,
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's repository came from shell setup that the recording lacks, so the scenario's tables and remote are missing here"#)),
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
            name: r#"relkey"#,

            intent: r#"Doltlite oracle case: relkey."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'one');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b','release-2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'release-2'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (2, 'two');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c2 on release-2');"#,
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
                    query: r#"UPDATE example SET value='ONE' WHERE id=1;"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c3 on release-2');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b','bar');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'bar'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (9, 'unpushed');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c4 on bar');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('release-2');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'release-2'"#)],
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_push('origin', 'release-2');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|release-2"#)],
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
                    query: r#"SELECT CONCAT('R|', id, '|', value) FROM example;"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|1|ONE"#)],
                            &[T(r#"R|2|two"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "s7",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s7",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"twobranches"#,

            intent: r#"Doltlite oracle case: twobranches."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'base');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b','foo1');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'foo1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (2, 'foo1');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c2 on foo1');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b','bar');"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"status"#, INT8), Column(r#"message"#, TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'bar'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (3, 'bar');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c3 on bar');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_checkout('foo1');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_push('origin', 'foo1');"#,
                    expected: Expected::Ok,
                    client: "s5",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_checkout('bar');"#,
                    expected: Expected::Ok,
                    client: "s6",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_push('origin', 'bar');"#,
                    expected: Expected::Ok,
                    client: "s7",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s7",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"two_branch_fetch_first_checkout_contents"#,

            intent: r#"Doltlite oracle case: two branch fetch first checkout contents."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin','foo1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no remote"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('topic_foo','origin/foo1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: 'origin/foo1' is not a commit and a branch 'topic_foo' cannot be created from it"#, ..E }),
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
                    query: r#"SELECT CONCAT('R|', id, '|', value) FROM example AS OF 'topic_foo';"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found: topic_foo"#, ..E }),
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
            name: r#"two_branch_fetch_second_checkout_contents"#,

            intent: r#"Doltlite oracle case: two branch fetch second checkout contents."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin','bar');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no remote"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('topic_bar','origin/bar');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: 'origin/bar' is not a commit and a branch 'topic_bar' cannot be created from it"#, ..E }),
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
                    query: r#"SELECT CONCAT('R|', id, '|', value) FROM example AS OF 'topic_bar';"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found: topic_bar"#, ..E }),
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
            name: r#"two_branch_rename_first_branch"#,

            intent: r#"Doltlite oracle case: two branch rename first branch."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin','foo1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no remote"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('topic_foo','origin/foo1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: 'origin/foo1' is not a commit and a branch 'topic_foo' cannot be created from it"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('-m','topic_foo','renamed_foo');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found"#, ..E }),
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
                    query: r#"SELECT CONCAT('R|renamed|', count(*)) FROM dolt_branches WHERE name='renamed_foo' UNION ALL SELECT CONCAT('R|old|', count(*)) FROM dolt_branches WHERE name='topic_foo';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|renamed|0"#)],
                            &[T(r#"R|old|0"#)],
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
            name: r#"two_branch_delete_first_branch"#,

            intent: r#"Doltlite oracle case: two branch delete first branch."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin','foo1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no remote"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('topic_foo','origin/foo1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: 'origin/foo1' is not a commit and a branch 'topic_foo' cannot be created from it"#, ..E }),
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch('-D','topic_foo');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch not found"#, ..E }),
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
                    query: r#"SELECT CONCAT('R|deleted|', count(*)) FROM dolt_branches WHERE name='topic_foo';"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"concat"#, TEXT)],
                        rows: &[
                            &[T(r#"R|deleted|0"#)],
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
        ScriptTest {
            name: r#"clone_branch_zzz_main"#,

            intent: r#"Doltlite oracle case: clone branch zzz main."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'base');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_branch('zzz');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_push('origin', 'zzz');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "s5",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s7",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s7",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"clone_branch_zzz_master"#,

            intent: r#"Doltlite oracle case: clone branch zzz master."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'base');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_branch('zzz');"#,
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
                    query: r#"SELECT * FROM dolt_branch('master');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_push('origin', 'zzz');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_push('origin', 'master');"#,
                    expected: Expected::Ok,
                    client: "s5",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|master"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s7",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s7",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"clone_branch_zzz_master_main"#,

            intent: r#"Doltlite oracle case: clone branch zzz master main."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'base');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_branch('zzz');"#,
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
                    query: r#"SELECT * FROM dolt_branch('master');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_push('origin', 'zzz');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_push('origin', 'master');"#,
                    expected: Expected::Ok,
                    client: "s5",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "s6",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"clone_branch_zeta_alpha"#,

            intent: r#"Doltlite oracle case: clone branch zeta alpha."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'base');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_branch('zeta');"#,
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
                    query: r#"SELECT * FROM dolt_branch('alpha');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_push('origin', 'zeta');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_push('origin', 'alpha');"#,
                    expected: Expected::Ok,
                    client: "s5",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|alpha"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s7",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s7",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"clone_branch_alpha_zeta"#,

            intent: r#"Doltlite oracle case: clone branch alpha zeta."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'base');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_branch('alpha');"#,
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
                    query: r#"SELECT * FROM dolt_branch('zeta');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_push('origin', 'alpha');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_push('origin', 'zeta');"#,
                    expected: Expected::Ok,
                    client: "s5",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|alpha"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s7",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s7",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"clone_branch_gamma_beta_alpha"#,

            intent: r#"Doltlite oracle case: clone branch gamma beta alpha."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE example(id integer PRIMARY KEY, value TEXT NOT NULL);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO example VALUES (1, 'base');"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_commit('-A','-m','c1 base');"#,
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
                    query: r#"SELECT * FROM dolt_branch('gamma');"#,
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
                    query: r#"SELECT * FROM dolt_branch('beta');"#,
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
                    query: r#"SELECT * FROM dolt_branch('alpha');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{TEMPDIR}/drem');"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT dolt_push('origin', 'gamma');"#,
                    expected: Expected::Ok,
                    client: "s4",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s4",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_push('origin', 'beta');"#,
                    expected: Expected::Ok,
                    client: "s5",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_push('origin', 'alpha');"#,
                    expected: Expected::Ok,
                    client: "s6",
                    database: r#"db0"#,
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
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/drem', 'db1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('R|', active_branch());"#,
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T(r#"R|alpha"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s8",
                    database: r#"db1"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
    ]);
}
