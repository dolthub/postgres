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

//! Dolt's integration-tests/bats/remotes-file-system.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_remotes_file_system() {
    run_scripts(&[
        ScriptTest {
            name: r#"remotes-file-system: Add a file system based remote"#,
            intent: r#"A remote can name a directory by a file URL."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}/');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, url LIKE 'file://%/remote%' AS url FROM dolt_remotes;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("url", BOOL)],
                        rows: &[
                            &[T(r#"origin"#), T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes-file-system: Add a file system remote with a bad path"#,
            intent: r#"Adding a file remote records its URL, leaving its directory to the first push."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file:///poop/');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Bug(r#"Dolt makes a file remote's directory when it adds the remote, which its own GetAbsRemoteUrl TODO calls odd, so an uncreatable path fails the add, where Doltgrust records the URL and makes the directory at the first push"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes-file-system: push, pull, and clone file based remotes"#,
            intent: r#"A clone of a file remote pushes back a commit that the original then pulls."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
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
                    query: r#"SELECT dolt_commit('-m', 'test commit');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remotedir}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remotedir}', 'test-repo');"#,
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
                    query: r#"INSERT INTO test VALUES (0, 0, 0, 0, 0, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'put row');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
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
                    client: "test-repo",
                    database: "test-repo",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"put row"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt branch -v prints the head, so the port checks that the pull reached the clone's commit"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT c5 FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("c5", INT8)],
                        rows: &[
                            &[T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes-file-system: clone, fetch, and push from multiple file system remotes"#,
            intent: r#"Fetches name their remote until origin exists, and ref specs can store branches under any tracking name."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
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
                    query: r#"SELECT dolt_commit('-m', 'test commit');"#,
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
                    query: r#"SELECT dolt_remote('add', 'remote2', 'file://{NEWDIR:remote2}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('remote2', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unknown remote; 'origin' does not appear to be a dolt database. could not read from the remote database. please make sure you have the correct access rights and the database exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('remote2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'main:notmain');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
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
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin', 'refs/heads/notmain:refs/remotes/anything/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('remote2', 'refs/heads/main:refs/remotes/something/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_remote_branches WHERE name IN ('remotes/anything/main', 'remotes/something/main') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/anything/main"#)],
                            &[T(r#"remotes/something/main"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes-file-system: fetch displays and updates branch list"#,
            intent: r#"A clone lists every branch of the remote as a remote-tracking branch."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'tester');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'tester'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
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
                    query: r#"SELECT dolt_commit('-m', 'test commit');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'tester');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
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
                    query: r#"SELECT * FROM dolt_push('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote1}', 'test-repo');"#,
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
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"remotes/origin/main"#)],
                            &[T(r#"remotes/origin/tester"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "test-repo",
                    database: "test-repo",
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes-file-system: clone from a directory that is not a dolt repo"#,
            intent: r#"A clone from a directory holding no Dolt data fails and makes no database."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"COPY (SELECT 1) TO '{NEWDIR:remotedir}/some-junk';"#,
                    expected: Expected::Tag("COPY 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remotedir}', 'test-repo');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"clone failed; remote at that url contains no Dolt data"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_database WHERE datname = 'test-repo';"#,
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
