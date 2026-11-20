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

//! Dolt's integration-tests/bats/push.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_push() {
    run_scripts(&[
        ScriptTest {
            name: r#"push: push origin"#,
            intent: r#"A push to origin reaches a clone through a pull."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
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
            name: r#"push: reject invalid destination branch names"#,
            intent: r#"A push refuses destination names that aren't valid branch names, and accepts nested and fully qualified ones."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'main:/bad');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"not a valid user branch name: '/bad'"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main:////origin/bad');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"not a valid user branch name: '////origin/bad'"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main:bad..name');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"not a valid user branch name: 'bad..name'"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main:bad.lock');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"not a valid user branch name: 'bad.lock'"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main:bad/');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"not a valid user branch name: 'bad/'"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main:valid/nested');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'main:refs/heads/qualified');"#,
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
                    query: r#"SELECT * FROM dolt_fetch('origin');"#,
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
                    query: r#"SELECT count(*) FROM dolt_remote_branches WHERE name = 'remotes/origin/valid/nested';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/main"#)],
                            &[T(r#"remotes/origin/qualified"#)],
                            &[T(r#"remotes/origin/valid/nested"#)],
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
            name: r#"push: push custom remote"#,
            intent: r#"A push to another remote of the same address reaches a clone through a pull."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
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
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
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
            name: r#"push: push without repository defined throws error"#,
            intent: r#"A push names a remote first, and a branch with no upstream must be named."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('main');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: remote 'main' not found.
Please make sure the remote exists."#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: The current branch main has no upstream branch.
To push the current branch and set the remote as upstream, use
	dolt push --set-upstream origin main
To have this happen automatically for branches without a tracking
upstream, see 'push.autoSetupRemote' in 'dolt config --help'."#, ..E }),
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
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
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
            name: r#"push: push feature branch"#,
            intent: r#"A pushed feature branch reaches a clone through a fetch and checkout."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'feature');"#,
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
                    query: r#"SELECT * FROM dolt_fetch('origin', 'feature');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('feature');"#,
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
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
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
            name: r#"push: can set upstream branch with different name"#,
            intent: r#"A push with -u can name a different remote branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('-u', 'origin', 'other:remote-other');"#,
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
                            &[T(r#"remotes/origin/remote-other"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: push aborts if nothing specified, upstream has different name"#,
            intent: r#"A plain push refuses an upstream whose name differs from the branch's."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('-u', 'origin', 'other:remote-other');"#,
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
                    query: r#"SELECT * FROM dolt_push();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"the upstream branch of your current branch does not match the name of your current branch"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: push --set-upstream persists"#,
            intent: r#"A push with --set-upstream lets later plain pushes find the upstream."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: The current branch other has no upstream branch.
To push the current branch and set the remote as upstream, use
	dolt push --set-upstream origin other
To have this happen automatically for branches without a tracking
upstream, see 'push.autoSetupRemote' in 'dolt config --help'."#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('-u', 'origin', 'other');"#,
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
                    query: r#"SELECT * FROM dolt_push();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Everything up-to-date"#)],
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
            name: r#"push: push --force flag"#,
            intent: r#"A push behind the remote's tip is rejected and --force overwrites it."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"CREATE TABLE t2 (a INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
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
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'commit to override');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Error(Diagnostic { code: "XX000", message: r#"To file://{NEWDIR:rem1}
 ! [rejected]            main -> main (non-fast-forward)
error: failed to push some refs to 'file://{NEWDIR:rem1}'
hint: Updates were rejected because the tip of your current branch is behind
hint: its remote counterpart. Integrate the remote changes (e.g.
hint: 'dolt pull ...') before pushing again.
"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--force', 'origin', 'main');"#,
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
            name: r#"push: push to unknown remote"#,
            intent: r#"A push to a remote that isn't configured fails."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('unknown', 'main');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: remote 'unknown' not found.
Please make sure the remote exists."#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: push unknown branch"#,
            intent: r#"A push of a branch that doesn't exist fails."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'unknown');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid ref spec; refspec not found: 'unknown'; branch not found: unknown"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: not specifying a branch throws an error"#,
            intent: r#"A push with -u that names no branch fails when the branch has no upstream."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('-u', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: The current branch main has no upstream branch.
To push the current branch and set the remote as upstream, use
	dolt push --set-upstream origin main
To have this happen automatically for branches without a tracking
upstream, see 'push.autoSetupRemote' in 'dolt config --help'."#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: pushing empty branch does not panic"#,
            intent: r#"Pushing an empty ref spec fails without a panic."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', '');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid ref spec: ''"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: --silent suppresses progress message"#,
            intent: r#"dolt_push accepts --silent."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'main', '--silent');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"the progress message is the CLI's, so the port checks that the option is taken"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: push succeeds when file:// remote has ignored tables in working set"#,
            intent: r#"A clone of a repository with dolt_ignore rules pushes its new commit."#,
            set_up_script: &[
                r#"CREATE DATABASE local;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem2}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "local",
                    database: "local",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (id INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "local",
                    database: "local",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_ignore VALUES ('ignored_table', true);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "local",
                    database: "local",
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
                    client: "local",
                    database: "local",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Initial commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "local",
                    database: "local",
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
                    client: "local",
                    database: "local",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem2}', 'local2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "local2",
                    database: "local2",
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
                    client: "local2",
                    database: "local2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Second commit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "local2",
                    database: "local2",
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
                    client: "local2",
                    database: "local2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: push existing branch does not report new branch"#,
            intent: r#"A push that moves an existing remote branch reports the move, not a new branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT message ~ '[0-9a-v]+\.\.[0-9a-v]+\s+main -> main' AND message NOT LIKE '%[new branch]%' AS found FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::Differs(r#"Dolt's commit hashes are 32 characters, and Doltgrust's are longer, so the port matches hashes of any length"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: force push to existing diverged branch does not report new branch"#,
            intent: r#"A forced push over a diverged branch reports a forced update, not a new branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"CREATE TABLE diverged (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
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
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'diverged commit');"#,
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
                    query: r#"SELECT message ~ '\+\s[0-9a-v]+\.\.\.[0-9a-v]+\s+main -> main\s+\(forced update\)' AND message NOT LIKE '%[new branch]%' AS found FROM dolt_push('--force', 'origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::Differs(r#"Dolt's commit hashes are 32 characters, and Doltgrust's are longer, so the port matches hashes of any length"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"push: multi-branch push reports new branch only for new branches"#,
            intent: r#"A push of several ref specs reports a new branch only for the branch the remote lacked."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT dolt_branch('branch_new');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
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
                    query: r#"SELECT message ~ '\* \[new branch\]          branch_new -> branch_new' AND message NOT LIKE '%* [new branch]          main -> main%' AS found FROM dolt_push('origin', 'branch_new:branch_new', 'main:main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
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
            name: r#"push: push tag reports new tag"#,
            intent: r#"A push of a tag reports it as a new tag."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT message LIKE '%* [new tag]             v1 -> v1%' AS found FROM dolt_push('origin', 'v1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
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
            name: r#"push: delete remote branch reports deleted"#,
            intent: r#"A push that deletes a remote branch reports it deleted."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT dolt_branch('to_delete');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
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
                    query: r#"SELECT * FROM dolt_push('origin', 'to_delete');"#,
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
                    query: r#"SELECT message LIKE '%- [deleted]             to_delete%' AS found FROM dolt_push('origin', ':to_delete');"#,
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
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
            name: r#"push: push refspec with different destination name reports src to dest"#,
            intent: r#"A push to a differently named remote branch reports the source and destination, first as new and then as moved."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY, b INT);"#,
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
                    query: r#"SELECT dolt_commit('-am', 'First commit');"#,
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
                    query: r#"INSERT INTO t1 VALUES (0, 0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Second commit');"#,
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
                    query: r#"SELECT message LIKE '%* [new branch]          main -> foobar%' AS found FROM dolt_push('origin', 'main:foobar');"#,
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
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
                    query: r#"SELECT dolt_commit('-am', 'Commit for foobar');"#,
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
                    query: r#"SELECT message ~ '[0-9a-v]+\.\.[0-9a-v]+\s+main -> foobar' AS found FROM dolt_push('origin', 'main:foobar');"#,
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::Differs(r#"Dolt's commit hashes are 32 characters, and Doltgrust's are longer, so the port matches hashes of any length"#)),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
