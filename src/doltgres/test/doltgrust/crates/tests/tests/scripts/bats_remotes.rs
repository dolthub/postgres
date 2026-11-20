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

//! Dolt's integration-tests/bats/remotes.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_remotes() {
    run_scripts(&[
        ScriptTest {
            name: r#"remotes: cli 'dolt checkout new_branch' without -b flag creates new branch and sets upstream if there is a remote branch with matching name"#,
            intent: r#"Checking out a name only a remote has creates the local branch from it, tracking that remote branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"SELECT dolt_commit('--allow-empty', '-m', 'a commit for main from repo2');"#,
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
                    query: r#"SELECT * FROM dolt_push();"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'
branch 'other' set up to track 'origin/other'."#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches WHERE name = 'other';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"other"#), T(r#"origin"#), T(r#"other"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'Your branch is up to date' text is the CLI's, so the port reads the upstream dolt_branches records"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: guessing the remote branch fails if there are multiple remotes with branches with matching name"#,
            intent: r#"Checking out a name two remotes have a branch of is ambiguous and fails."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'other');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT name FROM dolt_remote_branches WHERE name LIKE '%other' ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/other"#)],
                            &[T(r#"remotes/test-remote/other"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('other');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"hint: If you meant to check out a remote tracking branch, on, e.g., 'origin',
hint: you can do so by fully qualifying the name with the --track option:
hint:
hint:        dolt checkout --track origin/<name>
'other' matched multiple (2) remote tracking branches"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: cli 'dolt checkout -b new_branch' should not set upstream if there is a remote branch with matching name"#,
            intent: r#"Creating a branch with -b leaves it untracked even when a remote has a branch of that name, so pulling it has no upstream."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"CREATE TABLE a (pk INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add table a');"#,
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
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
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
                    query: r#"SELECT name, remote, branch FROM dolt_branches WHERE name = 'other';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"other"#), T(r#""#), T(r#""#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'Your branch is up to date' text is the CLI's, so the port reads the upstream dolt_branches records"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'add table a');"#,
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: call dolt_checkout('new_branch') without '-b' sets upstream if there is a remote branch with matching name"#,
            intent: r#"dolt_checkout of a name only a fetched remote has creates a branch tracking it, which dolt_pull then pulls."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
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
                    query: r#"CREATE TABLE test (pk INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'main commit');"#,
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
                    query: r#"SELECT name FROM dolt_remote_branches WHERE name LIKE '%test-branch';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('--remote', 'test-remote', 'file://{NEWDIR:test-repo}', 'repo2');"#,
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
                            &[T(r#"remotes/test-remote/main"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'test-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'test-branch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'test commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('test-remote', 'test-branch');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('test-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'test-branch'
branch 'test-branch' set up to track 'test-remote/test-branch'."#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
                    query: r#"SELECT fast_forward, conflicts FROM dolt_pull();"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8)],
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"test-branch"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches WHERE name = 'test-branch';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"test-branch"#), T(r#"test-remote"#), T(r#"test-branch"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'Your branch is up to date' text is the CLI's, so the port reads the upstream dolt_branches records"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: select 'DOLT_CHECKOUT('-b','new_branch') should not set upstream if there is a remote branch with matching name"#,
            intent: r#"dolt_checkout('-b', ...) leaves the new branch untracked, so dolt_pull has no upstream."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"CREATE TABLE a (pk INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add table a');"#,
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
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: add a remote using dolt remote"#,
            intent: r#"dolt_remote('add', ...) records a remote, and one without a URL is refused."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT name FROM dolt_remotes;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"test-remote"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: invalid argument"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: remove a remote"#,
            intent: r#"dolt_remote('remove', ...) forgets a remote, and removing one that doesn't exist fails."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('remove', 'test-remote');"#,
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
                    query: r#"SELECT name FROM dolt_remotes;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('remove', 'poop');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: unknown remote: 'poop'"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone a remote"#,
            intent: r#"A clone of a pushed repository has its commit and nothing to commit."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"test commit"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone a complicated remote"#,
            intent: r#"A clone has the remote's branches and tags, and a backup of the clone carries its local branches and tags, not its remote-tracking ones, into a clone of the backup."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (1), (2), (3);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main:genesis-branch');"#,
                r#"SELECT dolt_tag('customtag', 'main');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'customtag');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}', 'clone-1');"#,
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
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/genesis-branch"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "clone-1",
                    database: "clone-1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"customtag"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clone-1",
                    database: "clone-1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'clone-1-branch', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'clone-1-branch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clone-1",
                    database: "clone-1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('add', 'mybackup', 'file://{NEWDIR:backup}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clone-1",
                    database: "clone-1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_backup('sync', 'mybackup');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clone-1",
                    database: "clone-1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:backup}', 'clone-2');"#,
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
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/clone-1-branch"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "clone-2",
                    database: "clone-2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tag_name FROM dolt_tags;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T(r#"customtag"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clone-2",
                    database: "clone-2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone a remote with docs"#,
            intent: r#"A clone carries the remote's dolt_docs rows and has nothing to commit."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', 'license-text'), ('README.md', 'readme-text');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'test doc commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"test doc commit"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT doc_name, doc_text FROM dolt_docs ORDER BY doc_name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR), Column("doc_text", TEXT)],
                        rows: &[
                            &[T(r#"LICENSE.md"#), T(r#"license-text"#)],
                            &[T(r#"README.md"#), T(r#"readme-text"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt docs print writes a doc to a file from the CLI, so the port reads the doc from dolt_docs"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone an empty remote"#,
            intent: r#"Cloning a remote that holds no Dolt data fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:empty}');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"clone failed; remote at that url contains no Dolt data"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone a non-existent remote"#,
            intent: r#"Cloning a remote URL where nothing exists fails."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{TEMPDIR}/foo/bar');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"clone failed; remote at that url contains no Dolt data"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone a different branch than main"#,
            intent: r#"dolt_clone('-b', ...) checks out the named branch and creates no local main."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_checkout('-b', 'test-branch');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'test-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('-b', 'test-branch', 'file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"test-branch"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"test commit"#)],
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
            name: r#"remotes: call a clone's remote something other than origin"#,
            intent: r#"dolt_clone('--remote', ...) names the clone's remote, which is then its only remote."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('--remote', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"test commit"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_remotes;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"test-remote"#)],
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
            name: r#"remotes: dolt fetch"#,
            intent: r#"dolt_fetch fetches a remote with its default or an explicit refspec, writing the remote branch a refspec names, and fails for an unknown remote."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('test-remote');"#,
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
                    query: r#"SELECT * FROM dolt_fetch('test-remote', 'refs/heads/main:refs/remotes/test-remote/main');"#,
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
                    query: r#"SELECT * FROM dolt_fetch('poop', 'refs/heads/main:refs/remotes/poop/main');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unknown remote; 'poop' does not appear to be a dolt database. could not read from the remote database. please make sure you have the correct access rights and the database exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('test-remote', 'refs/heads/main:refs/remotes/test-remote/poop');"#,
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
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/test-remote/main"#)],
                            &[T(r#"remotes/test-remote/poop"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: fetch output"#,
            intent: r#"A clone fetches the branches another clone pushed and the commits it added to existing ones."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT * FROM dolt_checkout('-b', 'branch1');"#,
                r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-Am', 'add some values to branch 1.');"#,
                r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'branch1');"#,
                r#"SELECT * FROM dolt_checkout('-b', 'branch2');"#,
                r#"INSERT INTO test (id) VALUES (4), (5), (6);"#,
                r#"SELECT dolt_commit('-Am', 'add some values to branch 2.');"#,
                r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'branch2');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}', 'test-repo2');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "test-repo",
                    database: "test-repo",
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
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'branch1', 'remotes/origin/branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id) VALUES (100), (101), (102);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add more values to branch 1.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'branch2', 'remotes/origin/branch2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id) VALUES (103), (104), (105);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add more values to branch 2.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'branch2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'branch3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id) VALUES (7), (8), (9);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add some values to branch 3.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'branch3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'branch4');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (id) VALUES (10), (11), (12);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add some values to  branch 4.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'branch4');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
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
                    client: "test-repo",
                    database: "test-repo",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt fetch's progress output is the CLI's, so the port reads what it fetched"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/branch1"#)],
                            &[T(r#"remotes/origin/branch2"#)],
                            &[T(r#"remotes/origin/branch3"#)],
                            &[T(r#"remotes/origin/branch4"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test AS OF 'remotes/origin/branch2';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"9"#)],
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
            name: r#"remotes: dolt fetch with docs"#,
            intent: r#"Fetching leaves a clone's own committed docs in its working state."#,
            set_up_script: &[
                r#"INSERT INTO dolt_docs VALUES ('LICENSE.md', 'initial-license'), ('README.md', 'initial-readme');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'initial doc commit');"#,
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('test-remote');"#,
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
                    query: r#"SELECT doc_name, doc_text FROM dolt_docs ORDER BY doc_name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR), Column("doc_text", TEXT)],
                        rows: &[
                            &[T(r#"LICENSE.md"#), T(r#"initial-license"#)],
                            &[T(r#"README.md"#), T(r#"initial-readme"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT doc_name, doc_text FROM dolt_docs ORDER BY doc_name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR), Column("doc_text", TEXT)],
                        rows: &[
                            &[T(r#"LICENSE.md"#), T(r#"initial-license"#)],
                            &[T(r#"README.md"#), T(r#"initial-readme"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_docs SET doc_text = 'dolt-repo-clones-' || lower(split_part(doc_name, '.', 1));"#,
                    expected: Expected::Tag("UPDATE 2"),
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'dolt-repo-clones updated docs');"#,
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
                    query: r#"UPDATE dolt_docs SET doc_text = doc_text || '-updated';"#,
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'update initial doc values in test-org/test-repo');"#,
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
                    query: r#"SELECT * FROM dolt_fetch('test-remote');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unknown remote; 'test-remote' does not appear to be a dolt database. could not read from the remote database. please make sure you have the correct access rights and the database exists"#, ..E }),
                    client: "test-repo",
                    database: "test-repo",
                    dolt: Some(Dolt::MySqlOnly(r#"the bats fetch ignores its failure, since the clone's remote is origin"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT doc_name, doc_text FROM dolt_docs ORDER BY doc_name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", VARCHAR), Column("doc_text", TEXT)],
                        rows: &[
                            &[T(r#"LICENSE.md"#), T(r#"dolt-repo-clones-license"#)],
                            &[T(r#"README.md"#), T(r#"dolt-repo-clones-readme"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: dolt merge with origin/main syntax."#,
            intent: r#"Merging origin/main does nothing until a fetch brings the remote's new commit, which it then fast-forwards to."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message = 'test commit';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
                    query: r#"SELECT * FROM dolt_merge('origin/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('origin/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"test commit"#)],
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
            name: r#"remotes: dolt fetch and merge with remotes/origin/main syntax"#,
            intent: r#"remotes/origin/main names the same remote-tracking branch for merging, before and after fetching main."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('remotes/origin/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
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
                    query: r#"SELECT * FROM dolt_merge('remotes/origin/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
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
            name: r#"remotes: generate a merge with no conflict with a remote branch"#,
            intent: r#"Pulling a remote's commit into a branch with its own commit makes a merge commit named for the remote branch."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'another test commit');"#,
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
                    query: r#"SELECT * FROM dolt_pull('origin', '--no-edit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message LIKE 'Merge branch ''main'' of %' AS merged FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("merged", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
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
            name: r#"remotes: merge with divergent head"#,
            intent: r#"Pulling a remote whose head was amended and force-pushed merges it rather than fast-forwarding."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"SELECT dolt_commit('--amend', '-m', 'new message');"#,
                r#"SELECT * FROM dolt_push('-f', 'test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message LIKE 'Merge branch ''main'' of %' AS merged FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("merged", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
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
            name: r#"remotes: generate a merge with a conflict with a remote branch"#,
            intent: r#"Pulling a remote's conflicting row stops with a conflict, which resolved and pushed back reaches the original repository's pull."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'created table');"#,
                r#"SELECT * FROM dolt_push('--set-upstream', 'test-remote', 'main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'row to generate conflict');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0, 1, 1, 1, 1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'conflicting row');"#,
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
                    query: r#"SET dolt_allow_commit_conflicts = on;"#,
                    expected: Expected::Tag("SET"),
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('origin');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
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
                    query: r#"SELECT dolt_commit('-m', 'Fixed conflicts');"#,
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
                    query: r#"SELECT * FROM dolt_pull('test-remote');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message = 'Fixed conflicts';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: r#"remotes: clone sets your current branch appropriately"#,
            intent: r#"A clone checks out the remote's first branch by name when the remote has no main, and main once it does."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_checkout('-b', 'aaa');"#,
                r#"SELECT * FROM dolt_checkout('-b', 'zzz');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'aaa');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'zzz');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"aaa"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"aaa"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
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
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}', 'test-repo2');"#,
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
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo2",
                    database: "test-repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: force fetch from main"#,
            intent: r#"A pull succeeds after another clone force-pushed main past the commit this repository pushed."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('--set-upstream', 'test-remote', 'main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'test commit');"#,
                r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE other (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, c3 BIGINT, c4 BIGINT, c5 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('other');"#,
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
                    query: r#"SELECT dolt_commit('-m', 'added other table');"#,
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
                    query: r#"SELECT * FROM dolt_fetch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
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
                    query: r#"SELECT * FROM dolt_push('-f', 'origin', 'main');"#,
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
                    query: r#"SELECT conflicts FROM dolt_pull('--no-edit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8)],
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
            name: r#"remotes: clone checks out main, if found"#,
            intent: r#"A clone checks out main when the remote has it."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:zzz');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:dev');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:prod');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:aaa');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'On branch' text is the CLI's, so the port reads active_branch() and dolt_status"#)),
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone checks out master, if main not found"#,
            intent: r#"A clone checks out master when the remote has no main."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:111');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:dev');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:master');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:prod');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:aaa');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"master"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'On branch' text is the CLI's, so the port reads active_branch() and dolt_status"#)),
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone checks out main, if both main and master found"#,
            intent: r#"A clone checks out main when the remote has it, even beside master."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:master');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:dev');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:456');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:aaa');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'On branch' text is the CLI's, so the port reads active_branch() and dolt_status"#)),
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone checks out first lexicographical branch if neither main nor master found"#,
            intent: r#"A clone checks out the remote's first branch by name when it has neither main nor master."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:123');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:dev');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:456');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:prod');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:aaa');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"123"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'On branch' text is the CLI's, so the port reads active_branch() and dolt_status"#)),
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone creates remotes refs for all remote branches"#,
            intent: r#"A clone has a local branch only for the branch it checks out, and a remote-tracking branch for each of the remote's branches."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_commit('--allow-empty', '-m', 'another commit.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-one');"#,
                r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-Am', 'add some values.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-two');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
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
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"remotes/origin/branch-one"#)],
                            &[T(r#"remotes/origin/branch-two"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone --single-branch does not create remote refs for all remote branches"#,
            intent: r#"dolt_clone('--single-branch', ...) tracks only the branch it checks out."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_commit('--allow-empty', '-m', 'another commit.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-one');"#,
                r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-Am', 'add some values.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-two');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('--single-branch', 'file://{NEWDIR:test-repo}');"#,
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
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone --branch specifies which branch to clone"#,
            intent: r#"dolt_clone('--branch', ...) checks out that branch alone and tracks every remote branch."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_commit('--allow-empty', '-m', 'another commit.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-one');"#,
                r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-Am', 'add some values.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-two');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('--branch', 'branch-one', 'file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"branch-one"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"branch-one"#)],
                            &[T(r#"remotes/origin/branch-one"#)],
                            &[T(r#"remotes/origin/branch-two"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone --single-branch --branch does not create all remote refs"#,
            intent: r#"dolt_clone('--branch', ..., '--single-branch') checks out and tracks that branch alone."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_commit('--allow-empty', '-m', 'another commit.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-one');"#,
                r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-Am', 'add some values.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-two');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('--branch', 'branch-one', '--single-branch', 'file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"branch-one"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"branch-one"#)],
                            &[T(r#"remotes/origin/branch-one"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: fetch creates new remote refs for new remote branches"#,
            intent: r#"A fetch adds a remote-tracking branch for each branch pushed to the remote since the clone."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('--allow-empty', '-m', 'another commit.');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'main:branch-one');"#,
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
                    query: r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add some values.');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'main:branch-two');"#,
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
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"remotes/origin/branch-one"#)],
                            &[T(r#"remotes/origin/branch-two"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: can use refs/remotes/origin/... as commit reference for log"#,
            intent: r#"refs/remotes/origin/main names the remote-tracking branch in dolt_log."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log('refs/remotes/origin/main') LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"create test table."#)],
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
            name: r#"remotes: can use refs/remotes/origin/... as commit reference for diff"#,
            intent: r#"refs/remotes/origin/main names the remote-tracking branch in a diff either way round."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('HEAD', 'refs/remotes/origin/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('refs/remotes/origin/main', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: can use refs/remotes/origin/... as commit reference for merge"#,
            intent: r#"refs/remotes/origin/main names the remote-tracking branch in dolt_merge."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('refs/remotes/origin/main', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
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
            name: r#"remotes: can use remotes/origin/... as commit reference for log"#,
            intent: r#"remotes/origin/main names the remote-tracking branch in dolt_log."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log('remotes/origin/main') LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"create test table."#)],
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
            name: r#"remotes: can use remotes/origin/... as commit reference for diff"#,
            intent: r#"remotes/origin/main names the remote-tracking branch in a diff either way round."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('HEAD', 'remotes/origin/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('remotes/origin/main', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: can use remotes/origin/... as commit reference for merge"#,
            intent: r#"remotes/origin/main names the remote-tracking branch in dolt_merge."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('remotes/origin/main', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
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
            name: r#"remotes: can use origin/... as commit reference for log"#,
            intent: r#"origin/main names the remote-tracking branch in dolt_log."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT message FROM dolt_log('origin/main') LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T(r#"create test table."#)],
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
            name: r#"remotes: can use origin/... as commit reference for diff"#,
            intent: r#"origin/main names the remote-tracking branch in a diff either way round."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('HEAD', 'origin/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('origin/main', 'HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: can use origin/... as commit reference for merge"#,
            intent: r#"origin/main names the remote-tracking branch in dolt_merge."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('origin/main', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
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
            name: r#"remotes: can delete remote reference branch as origin/..."#,
            intent: r#"dolt_branch('-r', '-d', ...) deletes remote-tracking branches named origin/..., one or several at a time."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"SELECT dolt_commit('--allow-empty', '-m', 'another commit.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-one');"#,
                r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-Am', 'add some values.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-two');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
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
                    query: r#"SELECT dolt_branch('-r', '-d', 'origin/main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
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
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"remotes/origin/branch-one"#)],
                            &[T(r#"remotes/origin/branch-two"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('-r', '-d', 'origin/branch-one', 'origin/branch-two');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
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
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
            name: r#"remotes: can list remote reference branches with -r"#,
            intent: r#"Remote-tracking branches list apart from local ones, and together with them."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                r#"SELECT dolt_commit('--allow-empty', '-m', 'another commit.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-one');"#,
                r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-Am', 'add some values.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:branch-two');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
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
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/branch-one"#)],
                            &[T(r#"remotes/origin/branch-two"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt branch -r, dolt branch, and dolt branch -a list in the CLI, so the port reads dolt_remote_branches, dolt_branches, and both"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"remotes/origin/branch-one"#)],
                            &[T(r#"remotes/origin/branch-two"#)],
                            &[T(r#"remotes/origin/main"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: no remote is set"#,
            intent: r#"Pushing with no remote configured fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"fatal: No configured push destination.
Either specify the URL from the command-line or configure a remote repository using

	dolt remote add <name> <url>

and then push using the remote name

	dolt push <name>

"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: fetching unknown remotes should error"#,
            intent: r#"Fetching a name that is no remote fails as Dolt reports it, without a panic."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('remotes/dasdas');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unknown remote; 'remotes/dasdas' does not appear to be a dolt database. could not read from the remote database. please make sure you have the correct access rights and the database exists"#, ..E }),
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: fetching added invalid remote correctly errors"#,
            intent: r#"Fetching a DoltHub remote that doesn't exist fails with permission denied, without a panic."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'myremote', 'dolthub/fake');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
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
                    query: r#"SELECT * FROM dolt_fetch('myremote');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"permission denied"#, ..E }),
                    client: "test-repo",
                    database: "test-repo",
                    skip: Some(r#"Stage 3: DoltHub remotes aren't built, so fetching one fails with 0A000"#),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: fetching unknown remote ref errors accordingly"#,
            intent: r#"Fetching a ref the remote doesn't have fails, naming the ref as written."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('origin', 'dadasdfasdfa');"#,
                    expected: Expected::Error(Diagnostic { code: "XX000", message: r#"fetch failed: invalid ref spec: 'dadasdfasdfa'"#, ..E }),
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: checkout with -f flag without conflict"#,
            intent: r#"dolt_checkout('-f', ...) switches branches, each with its own rows."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                r#"INSERT INTO test (id) VALUES (8);"#,
                r#"SELECT dolt_commit('-Am', 'create test table.');"#,
                r#"SELECT * FROM dolt_push('origin', 'main:main');"#,
                r#"SELECT * FROM dolt_checkout('-b', 'branch1');"#,
                r#"INSERT INTO test (id) VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-Am', 'add some values to branch 1.');"#,
                r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'branch1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-f', 'main');"#,
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
                    query: r#"SELECT * FROM test ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T(r#"8"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'branch1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                            &[T(r#"3"#)],
                            &[T(r#"8"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: clone sets default upstream for main"#,
            intent: r#"A clone's main tracks the remote's, so a push with no arguments succeeds."#,
            set_up_script: &[
                r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
                r#"SELECT * FROM dolt_push('origin', 'main');"#,
                r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push();"#,
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: local clone does not contain working set changes"#,
            intent: r#"A clone has the pushed commits, not the source's uncommitted changes."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (i INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("status", TEXT)],
                        rows: &[
                            &[T(r#"public.t"#), T(r#"new table"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'new table');"#,
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
                    query: r#"CREATE TABLE t2 (i INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'rem1', 'file://{NEWDIR:rem1}');"#,
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
                    query: r#"SELECT * FROM dolt_push('rem1', 'main');"#,
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
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: dolt_remote add and remove works with other commands"#,
            intent: r#"A remote added with dolt_remote serves pushes, clones, and pulls of new branches, and pulling after it is removed fails."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:test-repo}');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}', 'repo2');"#,
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
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
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
                    query: r#"SELECT * FROM dolt_push('origin', 'other');"#,
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
                            &[T(r#"0"#), T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                            &[T(r#"remotes/origin/main"#)],
                            &[T(r#"remotes/origin/other"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add table a');"#,
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
                    query: r#"SELECT * FROM dolt_push();"#,
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
                    query: r#"SELECT dolt_remote('remove', 'origin');"#,
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
                    query: r#"SELECT * FROM dolt_pull();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"no remote"#, ..E }),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: dolt status on local repo compares with remote tracking"#,
            intent: r#"A branch compares with its remote-tracking branch: even, behind after a fetch, diverged after a local commit, and ahead after a pull."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"CREATE TABLE test (id INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'create table');"#,
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
                    query: r#"SELECT * FROM dolt_push();"#,
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
                    query: r#"SELECT * FROM dolt_branch_status('remotes/origin/main', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T(r#"main"#), T(r#"0"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's ahead, behind, and diverged text is the CLI's, so the port reads dolt_branch_status against the remote-tracking branch"#)),
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
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_status('remotes/origin/main', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T(r#"main"#), T(r#"0"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE different (id INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'create different table');"#,
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
                    query: r#"SELECT * FROM dolt_branch_status('remotes/origin/main', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T(r#"main"#), T(r#"1"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull('--no-edit');"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_status('remotes/origin/main', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", INT8), Column("commits_behind", INT8)],
                        rows: &[
                            &[T(r#"main"#), T(r#"2"#), T(r#"0"#)],
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
            name: r#"remotes: dolt checkout --track origin/feature checks out new local branch 'feature' with upstream set"#,
            intent: r#"dolt_checkout('--track', 'origin/feature') creates feature tracking the remote branch, which pulls as up to date."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('--track', 'origin/feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature'
branch 'feature' set up to track 'origin/feature'."#)],
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
            name: r#"remotes: call dolt_checkout track flag sets upstream"#,
            intent: r#"dolt_checkout('--track', ...) switches to the new tracking branch, which pulls as up to date."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"CREATE TABLE a (pk INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'add table a');"#,
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
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('--track', 'origin/other');"#,
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
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"other"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches WHERE name = 'other';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"other"#), T(r#"origin"#), T(r#"other"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::MySqlOnly(r#"dolt status's 'Your branch is up to date' text is the CLI's, so the port reads the upstream dolt_branches records"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_pull();"#,
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
            name: r#"remotes: call dolt_checkout with --track and no arg returns error"#,
            intent: r#"dolt_checkout('--track') without a branch fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('--track');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: no value for option `track'"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: call dolt_checkout with local branch name"#,
            intent: r#"dolt_checkout('--track', ...) of a name that is no remote branch fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('--track', 'newbranch');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid ref spec"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: call dolt_checkout('-b', 'newbranch', '--track', 'origin/feature') checks out new local branch 'newbranch' with upstream set"#,
            intent: r#"dolt_checkout('-b', ...) of a remote-tracking branch tracks it, with --track or without it, as branch.autosetupmerge defaults to tracking."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'newbranch', '--track', 'origin/feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'newbranch'
branch 'newbranch' set up to track 'origin/feature'."#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches WHERE name = 'newbranch';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"newbranch"#), T(r#"origin"#), T(r#"feature"#)],
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
                            &[T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
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
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('-D', 'newbranch');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'newbranch2', 'origin/feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'newbranch2'
branch 'newbranch2' set up to track 'origin/feature'."#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches WHERE name = 'newbranch2';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"newbranch2"#), T(r#"origin"#), T(r#"feature"#)],
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
            name: r#"remotes: dolt checkout -b newbranch --track origin/feature checks out new local branch 'newbranch' with upstream set"#,
            intent: r#"dolt_checkout('-b', ...) of a remote-tracking branch tracks it, with --track or without it, as branch.autosetupmerge defaults to tracking."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'newbranch', '--track', 'origin/feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'newbranch'
branch 'newbranch' set up to track 'origin/feature'."#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches WHERE name = 'newbranch';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"newbranch"#), T(r#"origin"#), T(r#"feature"#)],
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
                            &[T(r#"0"#), T(r#"0"#), T(r#"Everything up-to-date"#)],
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
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('-D', 'newbranch');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'newbranch', 'origin/feature');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'newbranch'
branch 'newbranch' set up to track 'origin/feature'."#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches WHERE name = 'newbranch';"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"newbranch"#), T(r#"origin"#), T(r#"feature"#)],
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
            name: r#"remotes: dolt_clone failure cleanup"#,
            intent: r#"A failed dolt_clone leaves no database behind."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file:///tmp/sanity/remote');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"clone failed; remote at that url contains no Dolt data"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_database WHERE datname = 'remote';"#,
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
            name: r#"remotes: dolt_clone procedure"#,
            intent: r#"dolt_clone checks its arguments, clones into a new database named after the remote or as given, refuses a name a database has, and takes -remote and -branch."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT dolt_clone();"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: invalid number of arguments: database URL must be specified and database name is optional"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file:///tmp/sanity/remote', 'foo', 'bar');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: clone has too many positional arguments. Expected at most 2, found 3: file:///tmp/sanity/remote, foo, bar"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}');"#,
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
                    query: r#"SELECT name FROM dolt_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "remote",
                    database: "remote",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_remote_branches ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/main"#)],
                            &[T(r#"remotes/origin/other"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "remote",
                    database: "remote",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}');"#,
                    expected: Expected::Error(Diagnostic { code: "42P04", message: r#"can't create database remote; database exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname = 'remote';"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(r#"remote"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'repo2');"#,
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
                    query: r#"CREATE TABLE new_table (a INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO new_table VALUES (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'a commit for main from repo2');"#,
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
                    query: r#"SELECT dolt_clone('-remote', 'custom', 'file://{NEWDIR:remote}', 'custom_remote');"#,
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
                    query: r#"SELECT name FROM dolt_remotes;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"custom"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "custom_remote",
                    database: "custom_remote",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('-branch', 'other', 'file://{NEWDIR:remote}', 'single_branch');"#,
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
                    query: r#"SELECT name FROM dolt_branches;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"other"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "single_branch",
                    database: "single_branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"other"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "single_branch",
                    database: "single_branch",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: fetch --prune deletes remote refs not on remote"#,
            intent: r#"dolt_fetch('--prune') removes remote-tracking branches the fetched remote no longer has, leaving other remotes' alone, and refuses a ref spec."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote1}');"#,
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
                    query: r#"SELECT dolt_remote('add', 'remote2', 'file://{NEWDIR:remote2}');"#,
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
                    query: r#"SELECT dolt_branch('b1');"#,
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
                    query: r#"SELECT dolt_branch('b2');"#,
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
                    query: r#"SELECT * FROM dolt_push('remote2', 'main');"#,
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
                    query: r#"SELECT * FROM dolt_push('origin', 'b1');"#,
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
                    query: r#"SELECT * FROM dolt_push('remote2', 'b2');"#,
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
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote1}', 'repo2');"#,
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
                    query: r#"SELECT dolt_remote('add', 'remote2', 'file://{NEWDIR:remote2}');"#,
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
                    query: r#"SELECT * FROM dolt_fetch('remote2');"#,
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
                    query: r#"SELECT name FROM dolt_remote_branches WHERE name LIKE '%b_' ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/origin/b1"#)],
                            &[T(r#"remotes/remote2/b2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', ':b1');"#,
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
                    query: r#"SELECT * FROM dolt_push('remote2', ':b2');"#,
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
                    query: r#"SELECT * FROM dolt_fetch('--prune');"#,
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
                    query: r#"SELECT name FROM dolt_remote_branches WHERE name LIKE '%b_' ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T(r#"remotes/remote2/b2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('--prune', 'remote2');"#,
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
                    query: r#"SELECT name FROM dolt_remote_branches WHERE name LIKE '%b_' ORDER BY name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_fetch('--prune', 'remote2', 'refs/heads/main:refs/remotes/remote2/othermain');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"--prune option cannot be provided with a ref spec"#, ..E }),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: push ignores local tracking branch"#,
            intent: r#"Pushing a branch the remote deleted creates it again, whatever the local remote-tracking branch says."#,
            set_up_script: &[
                r#"CREATE DATABASE cloneA;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clonea",
                    database: "clonea",
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
                    client: "clonea",
                    database: "clonea",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'alt');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'alt'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clonea",
                    database: "clonea",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'new', '--allow-empty');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clonea",
                    database: "clonea",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', 'alt:alt');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clonea",
                    database: "clonea",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}', 'cloneb');"#,
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
                    query: r#"SELECT * FROM dolt_merge('origin/alt');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "cloneb",
                    database: "cloneb",
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
                    client: "cloneb",
                    database: "cloneb",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('origin', ':alt');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "cloneb",
                    database: "cloneb",
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
                    client: "clonea",
                    database: "clonea",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-B', 'alt');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'alt'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clonea",
                    database: "clonea",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'another', '--allow-empty');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clonea",
                    database: "clonea",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT message LIKE '%%[new branch]%%' AS new_branch FROM dolt_push('origin', 'alt:alt');"#,
                    expected: Expected::Rows {
                        columns: &[Column("new_branch", BOOL)],
                        rows: &[
                            &[T(r#"t"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "clonea",
                    database: "clonea",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remotes: add remote with invalid name fails"#,
            intent: r#"A remote name with a space, a slash, or another special character is refused."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'remote with space', 'file://{NEWDIR:test-repo}');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"remote name invalid"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'remote/slash', 'file://{NEWDIR:test-repo}');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"remote name invalid"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'remote@special', 'file://{NEWDIR:test-repo}');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"remote name invalid"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
