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

//! Dolt's integration-tests/bats/pull.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_pull() {
    run_scripts(&[
        ScriptTest {
            name: r#"pull: pull main"#,
            intent: r#"Pulling origin brings main's new table and commits."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"show tables and dolt log are the CLI's, so the port reads information_schema.tables and dolt_log"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('First commit', 'Second commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"First commit"#)],
                            &[T(r#"Second commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull custom remote"#,
            intent: r#"Pulling a remote other than origin brings the same commits."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('test-remote');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"show tables and dolt log are the CLI's, so the port reads information_schema.tables and dolt_log"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('First commit', 'Second commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"First commit"#)],
                            &[T(r#"Second commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull default origin"#,
            intent: r#"Pulling with no remote named pulls the branch's upstream on origin."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('remove', 'test-remote');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"show tables and dolt log are the CLI's, so the port reads information_schema.tables and dolt_log"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('First commit', 'Second commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"First commit"#)],
                            &[T(r#"Second commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull default custom remote"#,
            intent: r#"A branch whose upstream's remote was removed has no tracking information until its upstream is set to another remote's branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('remove', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"There is no tracking information for the current branch.
Please specify which branch you want to merge with.

	dolt pull <remote> <branch>

If you wish to set tracking information for this branch you can do so with:

	 dolt push --set-upstream <remote> <branch>
"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('test-remote');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('--set-upstream-to', 'test-remote/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"show tables and dolt log are the CLI's, so the port reads information_schema.tables and dolt_log"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('First commit', 'Second commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"First commit"#)],
                            &[T(r#"Second commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull up to date does not error"#,
            intent: r#"Pulling again once up to date succeeds and says so."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull unknown remote fails"#,
            intent: r#"Pulling from a remote that isn't configured fails without a panic."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('unknown');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: remote 'unknown' not found.
Please make sure the remote exists."#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull unknown feature branch fails"#,
            intent: r#"Pulling a remote on a branch with no upstream and no branch named fails without a panic."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"You asked to pull from the remote 'origin', but did not specify a branch. Because this is not the default configured remote for your current branch, you must specify a branch."#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull feature branch"#,
            intent: r#"Pulling a feature branch with an upstream brings what was merged into it on the remote."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"show tables and dolt log are the CLI's, so the port reads information_schema.tables and dolt_log"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('First commit', 'Second commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"First commit"#)],
                            &[T(r#"Second commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: checkout after fetch a new feature branch"#,
            intent: r#"A branch fetched from the remote checks out with its own table and commits."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'feature2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature2'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t2 (i INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
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
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'create t2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'feature2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin', 'feature2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                            &[T(r#"t2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"show tables and dolt log are the CLI's, so the port reads information_schema.tables and dolt_log"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('First commit', 'Second commit', 'create t2') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"First commit"#)],
                            &[T(r#"Second commit"#)],
                            &[T(r#"create t2"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull force"#,
            intent: r#"A pull that leaves a foreign key violated fails, and --force commits the merge anyway."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE colors (id INT NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE UNIQUE INDEX color_index ON colors (color);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE objects (id INT NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'Commit1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE objects ADD CONSTRAINT color FOREIGN KEY (color) REFERENCES colors (color);"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'Commit2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO objects (id, name, color) VALUES (1, 'truck', 'red'), (2, 'ball', 'green'), (3, 'shoe', 'blue');"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-A', '-m', 'Commit3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"Committing this transaction resulted in a working set with constraint violations, transaction rolled back. This constraint violation may be the result of a previous merge or the result of transaction sequencing. Constraint violations from a merge can be resolved using the dolt_constraint_violations table before committing the transaction. To allow transactions to be committed with constraint violations from a merge or transaction sequencing set dolt_force_transaction_commit to on.
Constraint violations: 
Type: Foreign Key Constraint Violation
	ForeignKey: color,
	Table: objects,
	ReferencedTable: colors,
	Index: ,
	ReferencedIndex: color_index (3 row(s))"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"Dolt's violation text names the foreign key's own index, which Postgres doesn't need, so the port's Index is empty"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--force');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM objects ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", VARCHAR)],
                        rows: &[
                            &[T(r#"truck"#)],
                            &[T(r#"ball"#)],
                            &[T(r#"shoe"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('First commit', 'Second commit', 'Commit1', 'Commit3') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"Commit1"#)],
                            &[T(r#"Commit3"#)],
                            &[T(r#"First commit"#)],
                            &[T(r#"Second commit"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull squash"#,
            intent: r#"dolt_pull('--squash', ...) brings the remote's changes into the working set without committing them or its commits."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t2 (i INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'commit 1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--squash', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                            &[T(r#"t2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge branch%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::Bug(r#"Dolt's dolt_pull('--squash', ...) commits a three-way squash as a merge commit, where its documentation says --squash merges changes to the working set without updating the commit history"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message IN ('First commit', 'Second commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --noff flag"#,
            intent: r#"dolt_pull('--no-ff', ...) makes a merge commit where it could fast-forward."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--no-ff', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message LIKE 'Merge branch ''main''%' AS merged FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("merged", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull dirty working set fails"#,
            intent: r#"A pull that would change rows with uncommitted changes fails."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (3, 3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'dirty commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (2, 2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot merge with uncommitted changes"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull tag"#,
            intent: r#"A pull brings tags pushed to the remote."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"v1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull tags only for resolved commits"#,
            intent: r#"A pull brings the tags of commits it fetches, not those of commits on branches it doesn't."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v2', 'HEAD^');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'v2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t2 (a INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
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
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'feature commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'v3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"v1"#)],
                            &[T(r#"v2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull with remote and remote ref"#,
            intent: r#"Pulling a named remote branch merges it, fails for a branch the remote lacks, refuses uncommitted changes it would overwrite, and stops at a conflict."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'newbranch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'newbranch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', 'doesnotexist');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"branch "doesnotexist" not found on remote"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard', 'HEAD^1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 100);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', 'main');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot merge with uncommitted changes"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'adding new t1 table');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', 'main');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt pull's CONFLICT and Auto-merging lines are the CLI's, where dolt_pull fails the autocommit transaction"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"adding new t1 table"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull also fetches, but does not merge other branches"#,
            intent: r#"A pull fetches every remote branch but merges only the current branch's upstream."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
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
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('--allow-empty', '-m', 'new commit on other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message = 'new commit on other';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_status('remotes/origin/other', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T(r#"other"#), T(r#"0"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'behind' text is the CLI's, so the port reads dolt_branch_status"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull commits successful merge on current branch"#,
            intent: r#"Pulling a branch that diverged from its upstream commits the merge, named for the upstream branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
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
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'add (1,2) to t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t1 WHERE a = 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (2, 3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'add (2,3) to t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message LIKE 'Merge branch ''other'' of %' AS merged FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("merged", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: --no-ff and --no-commit"#,
            intent: r#"dolt_pull('--no-ff', '--no-commit', ...) merges into the working set without committing, for a later commit."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--no-ff', '--no-commit', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'merge from origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"merge from origin"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: --silent suppresses progress message"#,
            intent: r#"dolt_pull accepts --silent."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', '--silent');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt pull's progress message is the CLI's, so the port checks that the option is taken"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull when there are changes to ignored tables"#,
            intent: r#"Changes to an ignored table don't keep a pull from merging."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE ignore_this (id INT PRIMARY KEY, words VARCHAR(100));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_ignore VALUES ('ignore_*', true);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'Added ignore_this table and added a rule in dolt_ignore');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE testtable (id INT PRIMARY KEY, words VARCHAR(100));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO testtable VALUES (1, 'hello');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'Added test table');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO ignore_this VALUES (2, 'world');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM information_schema.tables WHERE table_name = 'testtable';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: clean up branches with --prune"#,
            intent: r#"dolt_pull('--prune') fast-forwards and removes remote-tracking branches the remote deleted."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
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
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('--allow-empty', '-m', 'new commit on other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'other:new-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/main"#)],
                            &[T(r#"remotes/origin/new-branch"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/main"#)],
                            &[T(r#"remotes/origin/new-branch"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('origin/new-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', ':new-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
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
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', 'main', '--prune');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"a SQL session can't delete its own checked-out branch to recreate it tracking origin/main, so the port names the branch to pull"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'HEAD:head-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: --ff-only succeeds when fast-forward is possible"#,
            intent: r#"dolt_pull('--ff-only', ...) fast-forwards when it can."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--ff-only', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: --ff-only fails when fast-forward is not possible"#,
            intent: r#"dolt_pull('--ff-only', ...) fails when the branch diverged."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE divergent (id INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'local divergent commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--ff-only', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: Not possible to fast-forward, aborting"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: --ff-only conflicts with --no-ff"#,
            intent: r#"--ff-only and --no-ff can't be given together."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--ff-only', '--no-ff', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: Flags '--ff-only' and '--no-ff' cannot be used together"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: --ff-only conflicts with --squash"#,
            intent: r#"--ff-only and --squash can't be given together."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--ff-only', '--squash', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: Flags '--ff-only' and '--squash' cannot be used together"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: --ff-only with already up-to-date branch"#,
            intent: r#"dolt_pull('--ff-only', ...) on a branch that is up to date succeeds."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--ff-only', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: --ff-only works with --no-commit"#,
            intent: r#"dolt_pull('--ff-only', '--no-commit', ...) fast-forwards."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--ff-only', '--no-commit', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with divergent history produces linear history"#,
            intent: r#"dolt_pull('--rebase', ...) replays local commits on the remote's, with no merge commit."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (2, 2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local commit', 'remote commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit"#)],
                            &[T(r#"remote commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#)],
                            &[T(r#"1"#), T(r#"1"#)],
                            &[T(r#"2"#), T(r#"2"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with no local commits fast-forwards"#,
            intent: r#"dolt_pull('--rebase', ...) with nothing local fast-forwards."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('First commit', 'Second commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"First commit"#)],
                            &[T(r#"Second commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase when already up-to-date"#,
            intent: r#"dolt_pull('--rebase', ...) on an up-to-date branch reports it."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with data conflict pauses rebase"#,
            intent: r#"A conflicting local commit stops dolt_pull('--rebase', ...) for resolution, and dolt_rebase('--continue') finishes it."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local conflicting commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts, message LIKE 'data conflict detected while rebasing commit % (local conflicting commit).%' AS conflict FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_pull returns a rebase's stop at a data conflict as its row, as Appendix C records for dolt_rebase"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local conflicting commit', 'remote commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local conflicting commit"#)],
                            &[T(r#"remote commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase conflicts with --squash"#,
            intent: r#"--rebase and --squash can't be given together."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', '--squash', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: Flags '--rebase' and '--squash' cannot be used together"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase conflicts with --no-ff"#,
            intent: r#"--rebase and --no-ff can't be given together."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', '--no-ff', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: Flags '--rebase' and '--no-ff' cannot be used together"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase conflicts with --ff-only"#,
            intent: r#"--rebase and --ff-only can't be given together."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', '--ff-only', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: Flags '--rebase' and '--ff-only' cannot be used together"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase conflicts with --no-commit"#,
            intent: r#"--rebase and --no-commit can't be given together."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', '--no-commit', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: Flags '--rebase' and '--no-commit' cannot be used together"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull -r shorthand works"#,
            intent: r#"-r is short for --rebase."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (2, 2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('-r', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local commit', 'remote commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit"#)],
                            &[T(r#"remote commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with multiple local commits preserves order"#,
            intent: r#"dolt_pull('--rebase', ...) replays several local commits in their order."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (10, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (3, 3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit 1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (4, 4);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit 2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (5, 5);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit 3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local commit 1', 'local commit 2', 'local commit 3', 'remote commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit 1"#)],
                            &[T(r#"local commit 2"#)],
                            &[T(r#"local commit 3"#)],
                            &[T(r#"remote commit"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE commit_hash = dolt_hashof('HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit 3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE commit_hash = dolt_hashof('HEAD~1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit 2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE commit_hash = dolt_hashof('HEAD~2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit 1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE commit_hash = dolt_hashof('HEAD~3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"remote commit"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#)],
                            &[T(r#"3"#), T(r#"3"#)],
                            &[T(r#"4"#), T(r#"4"#)],
                            &[T(r#"5"#), T(r#"5"#)],
                            &[T(r#"10"#), T(r#"10"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase conflict resolved with --theirs verifies data"#,
            intent: r#"Resolving a pull rebase's conflict with --theirs keeps the replayed local commit's row."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local conflicting commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts, message LIKE 'data conflict detected while rebasing commit % (local conflicting commit).%' AS conflict FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_pull returns a rebase's stop at a data conflict as its row, as Appendix C records for dolt_rebase"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#)],
                            &[T(r#"1"#), T(r#"99"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase conflict resolved with --ours keeps upstream data"#,
            intent: r#"Resolving a pull rebase's conflict with --ours keeps the upstream row."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local conflicting commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts, message LIKE 'data conflict detected while rebasing commit % (local conflicting commit).%' AS conflict FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_pull returns a rebase's stop at a data conflict as its row, as Appendix C records for dolt_rebase"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT b FROM t1 WHERE a = 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with multiple conflicts across multiple commits"#,
            intent: r#"A pull rebase stops at each conflicting commit in turn and cleans up its branch when done."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (2, 2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit with rows 1 and 2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit 1 conflicts on pk 1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (2, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit 2 conflicts on pk 2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts, message LIKE 'data conflict detected while rebasing commit % (local commit 1 conflicts on pk 1).%' AS conflict FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_pull returns a rebase's stop at a data conflict as its row, as Appendix C records for dolt_rebase"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT status, message LIKE 'data conflict detected while rebasing commit % (local commit 2 conflicts on pk 2).%' AS conflict FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_rebase returns its stop at a data conflict as its row, as Appendix C records"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local commit 1 conflicts on pk 1', 'local commit 2 conflicts on pk 2', 'remote commit with rows 1 and 2') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit 1 conflicts on pk 1"#)],
                            &[T(r#"local commit 2 conflicts on pk 2"#)],
                            &[T(r#"remote commit with rows 1 and 2"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_branches WHERE name = 'dolt_rebase_main';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with schema conflict auto-aborts"#,
            intent: r#"A schema conflict aborts a pull rebase, leaving no rebase in progress."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t1 ALTER COLUMN b TYPE VARCHAR(100);"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote schema change: modify b to varchar');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t1 ALTER COLUMN b TYPE BIGINT;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local schema change: modify b to bigint');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts, message LIKE 'schema conflict detected while rebasing commit %. the rebase has been automatically aborted' AS aborted FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("aborted", BOOL)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_pull returns a rebase's abort at a schema conflict as its row, as Appendix C records for dolt_rebase"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no rebase in progress"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_branches WHERE name = 'dolt_rebase_main';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with uncommitted changes fails"#,
            intent: r#"dolt_pull('--rebase', ...) refuses uncommitted changes and leaves them alone."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (3, 3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (4, 4);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local committed change');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (5, 5);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot start a rebase with uncommitted changes"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 WHERE a = 5;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T(r#"5"#), T(r#"5"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase when local is ahead returns up-to-date"#,
            intent: r#"dolt_pull('--rebase', ...) with only local commits reports up to date and keeps them."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (7, 7);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local only commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local only commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local only commit"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with multiple remote commits and one local"#,
            intent: r#"A pull rebase puts the local commit on top of every new remote commit."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (10, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit 1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (11, 11);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit 2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (12, 12);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit 3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (20, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local commit', 'remote commit 1', 'remote commit 2', 'remote commit 3') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit"#)],
                            &[T(r#"remote commit 1"#)],
                            &[T(r#"remote commit 2"#)],
                            &[T(r#"remote commit 3"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE commit_hash = dolt_hashof('HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#)],
                            &[T(r#"10"#), T(r#"10"#)],
                            &[T(r#"11"#), T(r#"11"#)],
                            &[T(r#"12"#), T(r#"12"#)],
                            &[T(r#"20"#), T(r#"20"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with non-conflicting new tables on both sides"#,
            intent: r#"A pull rebase keeps tables created on both sides."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t2 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'remote: create table t2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t3 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t3 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'local: create table t3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"t1"#)],
                            &[T(r#"t2"#)],
                            &[T(r#"t3"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local: create table t3', 'remote: create table t2') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local: create table t3"#)],
                            &[T(r#"remote: create table t2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase abort then retry"#,
            intent: r#"An aborted pull rebase returns to the branch, and a retry stops at the same conflict."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local conflicting commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts, message LIKE 'data conflict detected while rebasing commit % (local conflicting commit).%' AS conflict FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_pull returns a rebase's stop at a data conflict as its row, as Appendix C records for dolt_rebase"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Interactive rebase aborted"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_branches WHERE name = 'dolt_rebase_main';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts, message LIKE 'data conflict detected while rebasing commit % (local conflicting commit).%' AS conflict FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_pull returns a rebase's stop at a data conflict as its row, as Appendix C records for dolt_rebase"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_branches WHERE name = 'dolt_rebase_main';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase fails when dolt_rebase_main branch already exists"#,
            intent: r#"A pull rebase refuses to start when its working branch's name is taken, leaving the branch alone."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('dolt_rebase_main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (2, 2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: A branch named 'dolt_rebase_main' already exists."#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase shows conflicts during paused rebase"#,
            intent: r#"A paused pull rebase leaves the session on its working branch with the conflicts listed."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (1, 99);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local conflicting commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts, message LIKE 'data conflict detected while rebasing commit % (local conflicting commit).%' AS conflict FROM dolt_pull('--rebase', 'origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt_pull returns a rebase's stop at a data conflict as its row, as Appendix C records for dolt_rebase"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"dolt_rebase_main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_conflicts;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T(r#"public.t1"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_b, their_b FROM dolt_conflicts_t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_b", INT4), Column("their_b", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"99"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_t1"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Interactive rebase aborted"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_branches WHERE name = 'dolt_rebase_main';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"pull: pull --rebase with explicit remote and branch args"#,
            intent: r#"dolt_pull('--rebase', remote, branch) rebases onto that branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('feature');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Ok,
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'First commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Ok,
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (10, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'remote commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (20, 20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'local commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--rebase', 'origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"Successfully rebased and updated refs/heads/main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log WHERE message IN ('local commit', 'remote commit') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"local commit"#)],
                            &[T(r#"remote commit"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message LIKE 'Merge%';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#)],
                            &[T(r#"10"#), T(r#"10"#)],
                            &[T(r#"20"#), T(r#"20"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
