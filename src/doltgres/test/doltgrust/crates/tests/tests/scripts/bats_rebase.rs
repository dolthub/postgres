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

//! Dolt's integration-tests/bats/rebase.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_rebase() {
    run_scripts(&[
        ScriptTest {
            name: "rebase: no rebase in progress errors",
            intent: "--abort and --continue with no rebase in progress fail.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no rebase in progress", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no rebase in progress", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: non-interactive rebase works",
            intent: "A rebase onto another branch puts the branch's commits after that branch's.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branch_status('main', 'b1');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: bad args",
            intent: "dolt_rebase refuses missing, extra, unknown, and unresolvable arguments.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "not enough args", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "rebase takes at most one positional argument.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--abrot');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: unknown option `abrot'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'foo');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: foo", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: cannot rebase with dirty working set",
            intent: "A rebase over uncommitted changes fails.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot start a rebase with uncommitted changes", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: rebase working branch already exists",
            intent: "A rebase fails when its working branch's name is taken.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('dolt_rebase_b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'dolt_rebase_b1' already exists.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: verify custom script",
            intent: "An interactive rebase plans a pick of each commit and replays it.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT action, commit_message FROM dolt_rebase ORDER BY rebase_order;",
                    expected: Expected::Rows {
                        columns: &[Column("action", DOLT_REBASE_ACTION), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("pick"), T("b1 commit 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("b1 commit 1")],
                            &[T("main commit 2")],
                            &[T("main commit 1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: basic rebase",
            intent: "An interactive rebase with the default plan replays the branch's commits onto the target.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("b1 commit 1")],
                            &[T("main commit 2")],
                            &[T("main commit 1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: multi-line commit messages",
            intent: "A multi-line commit message survives a rebase.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', E'multi\\nline\\ncommit\\nmessage');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--empty=keep', '-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT action, commit_message FROM dolt_rebase ORDER BY rebase_order;",
                    expected: Expected::Rows {
                        columns: &[Column("action", DOLT_REBASE_ACTION), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("pick"), T("b1 commit 1")],
                            &[T("pick"), T("multi\nline\ncommit\nmessage")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("multi\nline\ncommit\nmessage")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: empty rebase plan aborts the rebase",
            intent: "A rebase whose plan is emptied aborts and cleans up.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_rebase;",
                    expected: Expected::Tag("DELETE 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("main commit 2")],
                            &[T("main commit 1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no rebase in progress", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b1")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: multi step rebase",
            intent: "A plan of pick, squash, drop, reword, and fixup rewrites the history as planned.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 4');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 5');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 6');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 7');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action = 'squash' WHERE commit_message IN ('b1 commit 2', 'b1 commit 3');",
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action = 'drop' WHERE commit_message = 'b1 commit 4';",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action = 'reword', commit_message = 'reworded!' WHERE commit_message = 'b1 commit 5';",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action = 'fixup' WHERE commit_message = 'b1 commit 6';",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("b1 commit 7")],
                            &[T("reworded!")],
                            &[T("b1 commit 1\n\nb1 commit 2\n\nb1 commit 3")],
                            &[T("main commit 2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: non-standard plan changes",
            intent: "A plan can drop commits and pick a commit from another branch.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'b2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'b2 commit 1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b2 commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3 VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b2 commit 3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.b1', dolt_hashof('b1'), false);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_rebase WHERE commit_message IN ('b2 commit 2', 'b2 commit 3');",
                    expected: Expected::Tag("DELETE 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_rebase (rebase_order, action, commit_hash, commit_message) VALUES (2, 'pick', current_setting('test.b1'), 'b1 commit 1');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("b1 commit 1")],
                            &[T("b2 commit 1")],
                            &[T("main commit 2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: rebase skips merge commits",
            intent: "A rebase leaves out merge commits.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'b1 merge commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'main commit 3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("b1 commit 2")],
                            &[T("b1 commit 1")],
                            &[T("main commit 3")],
                            &[T("main commit 2")],
                            &[T("main commit 1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: rebase with schema conflicts aborts",
            intent: "A schema conflict aborts the rebase and cleans up.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE boolean USING c <> 0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("a merge may change int to varchar, which Postgres converts in an assignment, so the schema conflict comes from a change to boolean, which Postgres converts only explicitly")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[&[T("1"), Any]],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust returns a rebase's stop at a conflict, and the abort a conflict forces, as a row with status 1 and Dolt's message instead of an error, since an error would roll back the stop or the abort (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no rebase in progress", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b1")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: rebase with commits that become empty",
            intent: "Commits that become empty are dropped by default and with --empty=drop, and kept with --empty=keep.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'repeating change from main on b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('testStartPoint');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("b1 commit 1")],
                            &[T("main commit 2")],
                            &[T("main commit 1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'testStartPoint');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', '--empty=drop', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("b1 commit 1")],
                            &[T("main commit 2")],
                            &[T("main commit 1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'testStartPoint');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', '--empty=keep', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("repeating change from main on b1")],
                            &[T("b1 commit 1")],
                            &[T("main commit 2")],
                            &[T("main commit 1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: edit action basic functionality",
            intent: "An edit action pauses the rebase on its working branch until --continue.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action = 'edit';",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens its plan in an editor, which is the CLI's, so the plan is the dolt_rebase table, read and changed with SQL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: invalid action forms should be rejected with clear error",
            intent: "An unknown action in the plan is refused.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action = 'invalid';",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input value for enum dolt_rebase_action: \"invalid\"", position: 33, ..E }),
                    dolt: Some(Dolt::Differs("the plan's action column is the built-in enum dolt_rebase_action, so a bad action fails with Postgres' own enum input error (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rebase: dolt rebase preserves ignored tables in the working set",
            intent: "A rebase keeps ignored tables in the working set.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c int);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "INSERT INTO t1 VALUES (1,1);",
                "SELECT dolt_commit('-am', 'main commit 2');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'b1 commit 1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_ignore VALUES ('private_data', true);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('dolt_ignore');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'add ignore rule for private_data');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE private_data (pk int PRIMARY KEY, secret varchar(100));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO private_data VALUES (1, 'secret');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, secret FROM private_data;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("secret", VARCHAR)],
                        rows: &[
                            &[T("1"), T("secret")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rebase: cannot rebase during active merge"#,
            intent: r#"A rebase can't start while a merge with conflicts is in progress."#,
            set_up_script: &[
                r#"CREATE TABLE t1 (pk int PRIMARY KEY, c int);"#,
                r#"SELECT dolt_commit('-Am', 'main commit 1');"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"INSERT INTO t1 VALUES (1,1);"#,
                r#"SELECT dolt_commit('-am', 'main commit 2');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"CREATE TABLE t2 (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'b1 commit 1');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"INSERT INTO t1 VALUES (1,2);"#,
                r#"SELECT dolt_add('t1');"#,
                r#"SELECT dolt_commit('-m', 'b1 commit 2');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::MySqlOnly("dolt merge is a CLI command, whose \"Automatic merge failed\" is dolt_merge's conflicts column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('-i', 'main');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unable to start rebase while a merge is in progress – abort the current merge before proceeding"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rebase: rebase with data conflict"#,
            intent: r#"An interactive rebase stops on the commit whose rows conflict, on its own branch, and continues to update the rebased branch once the conflict is resolved and staged."#,
            set_up_script: &[
                r#"CREATE TABLE t1 (pk int PRIMARY KEY, c int);"#,
                r#"SELECT dolt_commit('-Am', 'main commit 1');"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"INSERT INTO t1 VALUES (1,1);"#,
                r#"SELECT dolt_commit('-am', 'main commit 2');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"CREATE TABLE t2 (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'b1 commit 1');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"INSERT INTO t1 VALUES (1,2);"#,
                r#"SELECT dolt_commit('-am', 'b1 commit 2');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT status FROM dolt_rebase('-i', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens an editor on the plan, which the test leaves as it is, and dolt_rebase('-i') makes the plan for dolt_rebase('--continue') to run")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT status, message LIKE 'data conflict detected while rebasing commit % (b1 commit 2).%' AS conflict FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_c, their_pk, their_c FROM dolt_conflicts_t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT4), Column("our_c", INT4), Column("their_pk", INT4), Column("their_c", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_t1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b1")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rebase: rebase with data conflict non-interactive"#,
            intent: r#"A rebase stops on a conflicting commit, and a conflict resolved by hand with the conflict's row deleted continues to the resolved value."#,
            set_up_script: &[
                r#"CREATE TABLE t1 (pk int PRIMARY KEY, c int);"#,
                r#"SELECT dolt_commit('-Am', 'main commit 1');"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"INSERT INTO t1 VALUES (1,1);"#,
                r#"SELECT dolt_commit('-am', 'main commit 2');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"CREATE TABLE t2 (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'b1 commit 1');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"INSERT INTO t1 VALUES (1,23);"#,
                r#"SELECT dolt_commit('-am', 'b1 commit 2');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT status, message LIKE 'data conflict detected while rebasing commit % (b1 commit 2).%' AS conflict FROM dolt_rebase('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_conflicts;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_c, their_c FROM dolt_conflicts_t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_c", INT4), Column("their_c", INT4)],
                        rows: &[
                            &[T("1"), T("23")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_conflicts_t1;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t1 SET c = 42 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT c FROM t1 WHERE pk = 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("c", INT4)],
                        rows: &[
                            &[T("42")],
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
                            &[T("b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b1")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rebase: rebase with multiple data conflicts"#,
            intent: r#"A rebase stops on each conflicting commit in turn, refuses to continue while the resolution is unstaged, and finishes once the last conflict is resolved and staged."#,
            set_up_script: &[
                r#"CREATE TABLE t1 (pk int PRIMARY KEY, c int);"#,
                r#"SELECT dolt_commit('-Am', 'main commit 1');"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"INSERT INTO t1 VALUES (1,1);"#,
                r#"SELECT dolt_commit('-am', 'main commit 2');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"CREATE TABLE t2 (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'b1 commit 1');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"INSERT INTO t1 VALUES (2,200);"#,
                r#"SELECT dolt_commit('-am', 'main commit 3');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"INSERT INTO t1 VALUES (1,2);"#,
                r#"SELECT dolt_commit('-am', 'b1 commit 2');"#,
                r#"INSERT INTO t1 VALUES (2,3);"#,
                r#"SELECT dolt_commit('-am', 'b1 commit 3');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT status FROM dolt_rebase('-i', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens an editor on the plan, which the test leaves as it is, and dolt_rebase('-i') makes the plan for dolt_rebase('--continue') to run")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT status, message LIKE 'data conflict detected while rebasing commit % (b1 commit 2).%' AS conflict FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_c, their_pk, their_c FROM dolt_conflicts_t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT4), Column("our_c", INT4), Column("their_pk", INT4), Column("their_c", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_t1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot continue a rebase with unstaged changes. Use dolt_add() to stage tables and then continue the rebase"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT status, message LIKE 'data conflict detected while rebasing commit % (b1 commit 3).%' AS conflict FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_c, their_pk, their_c FROM dolt_conflicts_t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT4), Column("our_c", INT4), Column("their_pk", INT4), Column("their_c", INT4)],
                        rows: &[
                            &[T("2"), T("200"), T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_t1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b1")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rebase: complex edit and conflict scenario"#,
            intent: r#"A plan that edits the first and last commits and picks a conflicting one between them pauses at each edit, stops on the conflict, and ends with every commit and change in place."#,
            set_up_script: &[
                r#"CREATE TABLE t1 (pk int PRIMARY KEY, c int);"#,
                r#"SELECT dolt_commit('-Am', 'main commit 1');"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"INSERT INTO t1 VALUES (1,1);"#,
                r#"SELECT dolt_commit('-am', 'main commit 2');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"CREATE TABLE t2 (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_commit('-Am', 'b1 commit 1');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"INSERT INTO t1 VALUES (10, 100);"#,
                r#"SELECT dolt_commit('-am', 'main conflicting data');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"INSERT INTO t1 VALUES (5, 50);"#,
                r#"SELECT dolt_commit('-am', 'b1 commit 1 - to edit');"#,
                r#"INSERT INTO t1 VALUES (10, 200);"#,
                r#"SELECT dolt_commit('-am', 'b1 commit 2 - will conflict');"#,
                r#"INSERT INTO t1 VALUES (20, 300);"#,
                r#"SELECT dolt_commit('-am', 'b1 commit 3 - clean apply');"#,
                r#"INSERT INTO t1 VALUES (30, 400);"#,
                r#"SELECT dolt_commit('-am', 'b1 commit 4 - edit at end');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT status FROM dolt_rebase('-i', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i opens an editor on the plan, which the test leaves as it is, and dolt_rebase('-i') makes the plan for dolt_rebase('--continue') to run")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_rebase SET action = 'edit' WHERE commit_message IN ('b1 commit 1 - to edit', 'b1 commit 4 - edit at end');"#,
                    expected: Expected::Tag("UPDATE 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt rebase -i takes the plan from an editor, and dolt_rebase('-i') takes it from updates of the dolt_rebase table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dolt_rebase SET action = 'drop' WHERE commit_message = 'b1 commit 1';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("the test's plan file lists only the four commits it makes, which drops b1's first commit, and the port drops it in the dolt_rebase table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT status, message LIKE 'edit action paused at commit % (b1 commit 1 - to edit).%' AS paused FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("paused", BOOL)],
                        rows: &[
                            &[T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE t1 SET c = 55 WHERE pk = 5;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '--amend', '-m', 'edit modification to commit 1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (15, 150);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'edit modification to commit 1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT status, message LIKE 'data conflict detected while rebasing commit % (b1 commit 2 - will conflict).%' AS conflict FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("conflict", BOOL)],
                        rows: &[
                            &[T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_c, their_pk, their_c FROM dolt_conflicts_t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT4), Column("our_c", INT4), Column("their_pk", INT4), Column("their_c", INT4)],
                        rows: &[
                            &[T("10"), T("100"), T("10"), T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_t1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts resolve is a CLI command, which dolt_conflicts_resolve() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t1');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT status, message LIKE 'edit action paused at commit % (b1 commit 4 - edit at end).%' AS paused FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("paused", BOOL)],
                        rows: &[
                            &[T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_rebase('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt rebase is a CLI command, whose output is dolt_rebase's status and message, and the CLI keeps a rebase's conflicts as dolt_allow_commit_conflicts does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log('main..HEAD');"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log --oneline main..HEAD is a CLI command, so the port counts dolt_log('main..HEAD')")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("5"), T("55")],
                            &[T("10"), T("200")],
                            &[T("15"), T("150")],
                            &[T("20"), T("300")],
                            &[T("30"), T("400")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
