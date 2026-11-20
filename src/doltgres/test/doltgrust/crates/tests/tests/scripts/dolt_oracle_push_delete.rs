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

//! The `push_delete` cases of the retired Doltlite oracle corpus, with expectations recorded from Dolt's main branch.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_push_delete() {
    run_scripts(&[
        ScriptTest {
            name: r#"dt"#,

            intent: r#"Doltlite oracle case: dt."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t(id integer PRIMARY KEY);"#,
                    expected: Expected::Ok,
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES(1);"#,
                    expected: Expected::Ok,
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
                    query: r#"SELECT * FROM dolt_branch('other');"#,
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
                    query: r#"SELECT * FROM dolt_remote('add','origin','file://{TEMPDIR}_remote');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin','main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"To file://{TEMPDIR}_remote
 * [new branch]          main -> main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin','feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"To file://{TEMPDIR}_remote
 * [new branch]          feature -> feature"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s2",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin','other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"To file://{TEMPDIR}_remote
 * [new branch]          other -> other"#)],
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
                    query: r#"SELECT CONCAT('local|',name) AS n FROM dolt_branches
UNION ALL SELECT CONCAT('remote|',name) FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"n"#, TEXT)],
                        rows: &[
                            &[T(r#"local|feature"#)],
                            &[T(r#"local|main"#)],
                            &[T(r#"local|other"#)],
                            &[T(r#"remote|remotes/origin/feature"#)],
                            &[T(r#"remote|remotes/origin/main"#)],
                            &[T(r#"remote|remotes/origin/other"#)],
                        ],
                        tag: "SELECT 6",
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
                    query: r#"SELECT * FROM dolt_push('origin',':feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"To file://{TEMPDIR}_remote
 - [deleted]             feature"#)],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: r#"SELECT CONCAT('local|',name) AS n FROM dolt_branches
UNION ALL SELECT CONCAT('remote|',name) FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"n"#, TEXT)],
                        rows: &[
                            &[T(r#"local|feature"#)],
                            &[T(r#"local|main"#)],
                            &[T(r#"local|other"#)],
                            &[T(r#"remote|remotes/origin/main"#)],
                            &[T(r#"remote|remotes/origin/other"#)],
                        ],
                        tag: "SELECT 5",
                    },
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
                    query: r#"SELECT * FROM dolt_fetch('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: r#"SELECT CONCAT('local|',name) AS n FROM dolt_branches
UNION ALL SELECT CONCAT('remote|',name) FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"n"#, TEXT)],
                        rows: &[
                            &[T(r#"local|feature"#)],
                            &[T(r#"local|main"#)],
                            &[T(r#"local|other"#)],
                            &[T(r#"remote|remotes/origin/main"#)],
                            &[T(r#"remote|remotes/origin/other"#)],
                        ],
                        tag: "SELECT 5",
                    },
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
                    query: r#"SELECT * FROM dolt_push('origin',':feature');"#,
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"failed to delete remote; 'refs/heads/feature' from remote 'origin'; branch not found"#, ..E }),
                    client: "s8",
                    database: r#"db0"#,
                    skip: Some(r#"dolt_push deleting a remote branch the remote lacks fails with its own message, where Dolt says failed to delete remote; ... branch not found (bug, MacDG)"#),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s8",
                    close_client: true,
                    skip: Some(r#"dolt_push deleting a remote branch the remote lacks fails with its own message, where Dolt says failed to delete remote; ... branch not found (bug, MacDG)"#),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('local|',name) AS n FROM dolt_branches
UNION ALL SELECT CONCAT('remote|',name) FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"n"#, TEXT)],
                        rows: &[
                            &[T(r#"local|feature"#)],
                            &[T(r#"local|main"#)],
                            &[T(r#"local|other"#)],
                            &[T(r#"remote|remotes/origin/main"#)],
                            &[T(r#"remote|remotes/origin/other"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "s9",
                    database: r#"db0"#,
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
                    query: r#"SELECT * FROM dolt_push('origin','feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"To file://{TEMPDIR}_remote
 * [new branch]          feature -> feature"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s10",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s10",
                    database: r#"db0"#,
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
                    query: r#"SELECT CONCAT('local|',name) AS n FROM dolt_branches
UNION ALL SELECT CONCAT('remote|',name) FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"n"#, TEXT)],
                        rows: &[
                            &[T(r#"local|feature"#)],
                            &[T(r#"local|main"#)],
                            &[T(r#"local|other"#)],
                            &[T(r#"remote|remotes/origin/feature"#)],
                            &[T(r#"remote|remotes/origin/main"#)],
                            &[T(r#"remote|remotes/origin/other"#)],
                        ],
                        tag: "SELECT 6",
                    },
                    client: "s11",
                    database: r#"db0"#,
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
                    query: r#"SELECT CONCAT('local|',name) AS n FROM dolt_branches
UNION ALL SELECT CONCAT('remote|',name) FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"n"#, TEXT)],
                        rows: &[
                            &[T(r#"local|feature"#)],
                            &[T(r#"local|main"#)],
                            &[T(r#"local|other"#)],
                            &[T(r#"remote|remotes/origin/feature"#)],
                            &[T(r#"remote|remotes/origin/main"#)],
                            &[T(r#"remote|remotes/origin/other"#)],
                        ],
                        tag: "SELECT 6",
                    },
                    client: "s12",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s12",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin',':feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"To file://{TEMPDIR}_remote
 - [deleted]             feature"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s13",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s13",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT CONCAT('local|',name) AS n FROM dolt_branches
UNION ALL SELECT CONCAT('remote|',name) FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column(r#"n"#, TEXT)],
                        rows: &[
                            &[T(r#"local|feature"#)],
                            &[T(r#"local|main"#)],
                            &[T(r#"local|other"#)],
                            &[T(r#"remote|remotes/origin/main"#)],
                            &[T(r#"remote|remotes/origin/other"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "s14",
                    database: r#"db0"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#""#,
                    expected: Expected::Ok,
                    client: "s14",
                    close_client: true,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"tmp.RELPrO81Rb"#,

            intent: r#"Doltlite oracle case: tmp.RELPrO81Rb."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/dt_remote', 'db0');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"clone failed; remote at that url contains no Dolt data"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"peer"#,

            intent: r#"Doltlite oracle case: peer."#,
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "db0";"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin',':feature');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: remote 'origin' not found.
Please make sure the remote exists."#, ..E }),
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
