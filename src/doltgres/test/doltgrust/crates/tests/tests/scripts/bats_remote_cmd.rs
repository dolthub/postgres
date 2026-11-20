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

//! Dolt's integration-tests/bats/remote-cmd.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_remote_cmd() {
    run_scripts(&[
        ScriptTest {
            name: r#"remote-cmd: perform add"#,
            intent: r#"dolt_remote('add', ...) adds a remote that dolt_remotes lists with its URL."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'http://customhost/org/db');"#,
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
                    query: r#"SELECT name, url FROM dolt_remotes ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("url", TEXT)],
                        rows: &[
                            &[T(r#"origin"#), T(r#"http://customhost/org/db"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt remote -v is a CLI command, so the port reads dolt_remotes"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remote-cmd: stores normalized git+ssh url for scp-style input"#,
            intent: r#"An scp-style git address is stored as a git+ssh URL."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'git@github.com:org/repo.git');"#,
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
                    query: r#"SELECT name, url FROM dolt_remotes ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("url", TEXT)],
                        rows: &[
                            &[T(r#"origin"#), T(r#"git+ssh://git@github.com/./org/repo.git"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt remote -v is a CLI command, so the port reads dolt_remotes"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remote-cmd: stores normalized git+https url for https .git input"#,
            intent: r#"An https address ending in .git is stored as a git+https URL."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'other', 'https://example.com/org/repo.git');"#,
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
                    query: r#"SELECT name, url FROM dolt_remotes ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("url", TEXT)],
                        rows: &[
                            &[T(r#"other"#), T(r#"git+https://example.com/org/repo.git"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt remote -v is a CLI command, so the port reads dolt_remotes"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remote-cmd: perform re-add"#,
            intent: r#"Adding a remote whose name is taken fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'http://customhost/org/db');"#,
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
                    query: r#"SELECT dolt_remote('add', 'origin', 'http://otherhost/org/db');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"remote already exists"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remote-cmd: perform remove"#,
            intent: r#"dolt_remote('remove', ...) removes only the named remote."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'http://customhost/org/db');"#,
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
                    query: r#"SELECT dolt_remote('add', 'other', 'http://otherhost/org/db');"#,
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
                    query: r#"SELECT dolt_remote('remove', 'origin');"#,
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
                    query: r#"SELECT name, url FROM dolt_remotes ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("url", TEXT)],
                        rows: &[
                            &[T(r#"other"#), T(r#"http://otherhost/org/db"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt remote -v is a CLI command, so the port reads dolt_remotes"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remote-cmd: remove unsets branch upstream tracking"#,
            intent: r#"Removing a remote clears the upstreams of the branches that tracked it."#,
            set_up_script: &[
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
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'other', 'file://{NEWDIR:remote2}');"#,
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
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
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
                    query: r#"SELECT dolt_commit('-m', 'initial');"#,
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
                    query: r#"SELECT dolt_branch('b1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('-u', 'origin', 'b1:other');"#,
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
                    query: r#"SELECT dolt_branch('b2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('-u', 'other', 'b2:main');"#,
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
                    query: r#"SELECT name, remote, branch FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"b1"#), T(r#"origin"#), T(r#"other"#)],
                            &[T(r#"b2"#), T(r#"other"#), T(r#"main"#)],
                            &[T(r#"main"#), T(r#""#), T(r#""#)],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI's upstreams are in .dolt/repo_state.json, so the port reads dolt_branches"#)),
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
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"b1"#), T(r#""#), T(r#""#)],
                            &[T(r#"b2"#), T(r#"other"#), T(r#"main"#)],
                            &[T(r#"main"#), T(r#""#), T(r#""#)],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI's upstreams are in .dolt/repo_state.json, so the port reads dolt_branches"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remote-cmd: remove non-existent"#,
            intent: r#"Removing a remote that isn't configured fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('remove', 'origin');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: unknown remote: 'origin'"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"remote-cmd: remove origin and verify tracking is gone"#,
            intent: r#"Removing origin from a clone clears its branch's upstream."#,
            set_up_script: &[
                r#"CREATE DATABASE initter;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote_repo}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "initter",
                    database: "initter",
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
                    client: "initter",
                    database: "initter",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote_repo}', 'cloned_repo');"#,
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
                    query: r#"SELECT name, remote, branch FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"main"#), T(r#"origin"#), T(r#"main"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "cloned_repo",
                    database: "cloned_repo",
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI's upstreams are in .dolt/repo_state.json, so the port reads dolt_branches"#)),
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
                    client: "cloned_repo",
                    database: "cloned_repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, remote, branch FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(r#"main"#), T(r#""#), T(r#""#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "cloned_repo",
                    database: "cloned_repo",
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI's upstreams are in .dolt/repo_state.json, so the port reads dolt_branches"#)),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
