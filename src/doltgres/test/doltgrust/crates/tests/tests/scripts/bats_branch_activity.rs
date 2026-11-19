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

//! Dolt's integration-tests/bats/branch-activity.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_branch_activity() {
    run_scripts(&[
        ScriptTest {
            name: "branch-activity: last_read set for connections",
            intent: "Branches that sessions are connected to have a last read time.",
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, name varchar(50));",
                "INSERT INTO test VALUES (1, 'Alice'), (2, 'Bob');",
                "SELECT dolt_commit('-Am', 'Initial commit');",
                "SELECT dolt_branch('feature1');",
                "SELECT dolt_branch('feature2');",
                "SELECT dolt_branch('feature3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "f1",
                    database: "postgres/feature1",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "f2",
                    database: "postgres/feature2",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch FROM dolt_branch_activity WHERE last_read IS NOT NULL ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("feature1")],
                            &[T("feature2")],
                            &[T("main")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-activity: active session counts",
            intent: "dolt_branch_activity counts each branch's open sessions.",
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, name varchar(50));",
                "INSERT INTO test VALUES (1, 'Alice'), (2, 'Bob');",
                "SELECT dolt_commit('-Am', 'Initial commit');",
                "SELECT dolt_branch('feature1');",
                "SELECT dolt_branch('feature2');",
                "SELECT dolt_branch('feature3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "m",
                    database: "postgres/main",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "f1",
                    database: "postgres/feature1",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "f2a",
                    database: "postgres/feature2",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "f2b",
                    database: "postgres/feature2",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "f2c",
                    database: "postgres/feature2",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch, active_sessions FROM dolt_branch_activity WHERE last_read IS NOT NULL ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("active_sessions", INT4)],
                        rows: &[
                            &[T("feature1"), T("1")],
                            &[T("feature2"), T("3")],
                            &[T("main"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-activity: empty commit updates last_write",
            intent: "An empty commit sets the branch's last write time.",
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, name varchar(50));",
                "INSERT INTO test VALUES (1, 'Alice'), (2, 'Bob');",
                "SELECT dolt_commit('-Am', 'Initial commit');",
                "SELECT dolt_branch('feature1');",
                "SELECT dolt_branch('feature2');",
                "SELECT dolt_branch('feature3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT branch FROM dolt_branch_activity WHERE last_write IS NOT NULL AND branch = 'main' ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("The harness sets up through the server, whose writes give main a last write time, where Dolt's bats set up through the CLI before the server starts")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'empty commit', '--allow-empty');",
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
                    query: "SELECT branch FROM dolt_branch_activity WHERE last_write IS NOT NULL AND branch = 'main' ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-activity: data-changing commit updates last_write",
            intent: "A write to a branch sets its last write time.",
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, name varchar(50));",
                "INSERT INTO test VALUES (1, 'Alice'), (2, 'Bob');",
                "SELECT dolt_commit('-Am', 'Initial commit');",
                "SELECT dolt_branch('feature1');",
                "SELECT dolt_branch('feature2');",
                "SELECT dolt_branch('feature3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT branch FROM dolt_branch_activity WHERE last_write IS NOT NULL AND branch = 'main' ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("The harness sets up through the server, whose writes give main a last write time, where Dolt's bats set up through the CLI before the server starts")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 'Charlie');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch FROM dolt_branch_activity WHERE last_write IS NOT NULL AND branch = 'main' ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-activity: database isolation - activity in one database doesn't affect another",
            intent: "Each database's dolt_branch_activity lists only its own branches' activity.",
            server_config: "behavior:\n  branch_activity_tracking: true\n",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, name varchar(50));",
                "INSERT INTO test VALUES (1, 'Alice'), (2, 'Bob');",
                "SELECT dolt_commit('-Am', 'Initial commit');",
                "SELECT dolt_branch('feature1');",
                "SELECT dolt_branch('feature2');",
                "SELECT dolt_branch('feature3');",
                "CREATE DATABASE repo2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE items (id int PRIMARY KEY, description varchar(100));",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO items VALUES (100, 'Widget A'), (200, 'Widget B');",
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'Initial commit');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('dev');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('staging');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "f1",
                    database: "postgres/feature1",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "dev",
                    database: "repo2/dev",
                    dolt: Some(Dolt::MySqlOnly("An idle connection on a branch is a client that logged in to it and stays open")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch FROM dolt_branch_activity WHERE last_read IS NOT NULL ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("feature1")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch FROM dolt_branch_activity WHERE last_read IS NOT NULL ORDER BY branch;",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("dev")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-activity: error when tracking disabled",
            intent: "dolt_branch_activity is refused when the server doesn't track branch activity.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_activity;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "branch activity tracking is not enabled", hint: "Set \"behavior.branch_activity_tracking: true\" in the server's configuration.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
