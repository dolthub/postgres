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

//! Dolt's integration-tests/bats/branch.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_branch() {
    run_scripts(&[
        ScriptTest {
            name: "branch: deleting a branch deletes its working set",
            intent: "dolt_branch -d -f deletes a branch, which then no longer shows anywhere.",
            set_up_script: &[
                "SELECT dolt_branch('to_delete');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                            &[T("to_delete")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', '-f', 'to_delete');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
            name: "branch: moving current working branch takes its working set",
            intent: "Renaming the checked-out default branch keeps its uncommitted tables on it.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', '-f', 'main', 'new_main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust always runs as a server, and like Dolt's sql-server it refuses renaming the default branch without --force, so the port passes -f; Dolt skips this test (\"moves main branch which is not allowed with remote server\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("new_main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: deleting an unmerged branch with no remote",
            intent: "dolt_branch -d refuses a branch ahead of the checked-out branch and deletes even or behind ones, judged from the session's own branch.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'commit 1');",
                "CREATE TABLE t2 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'commit 2');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t3 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'new table');",
                "SELECT dolt_branch('b1-1');",
                "SELECT dolt_branch('b1-2');",
                "SELECT * FROM dolt_checkout('b3');",
                "SELECT dolt_reset('--hard', 'HEAD~');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch 'b1' is not fully merged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1-1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'b1-1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1-2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'b1-1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
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
            name: "branch: attempting to delete the currently checked out branch results in an error",
            intent: "Deleting the checked-out branch is refused.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to delete branch 'main', because it is the default branch for database 'postgres'; this can by changed on the command line, by stopping the sql-server, running `dolt checkout <another_branch> and restarting the sql-server", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust always runs as a server, and like Dolt's sql-server it refuses deleting the default branch, before and in place of the checked-out refusal; Dolt skips this test (\"deletes main branch which is not allowed within remote server\")")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: -- escapes arg parsing",
            intent: "-- ends the options, so a branch named like a flag can be made and deleted.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('--', '-b');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name = '-b';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', '-f', '--', '-b');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name = '-b';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: print nothing on successful create",
            intent: "dolt_branch makes a branch at HEAD or at a commit's hash.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('newbranch1', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.hash', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('newbranch2', current_setting('test.hash'));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                            &[T("newbranch1")],
                            &[T("newbranch2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: don't allow branch creation with HEAD or a commit id as a name",
            intent: "HEAD and a commit's hash are refused as names of new, renamed, and copied branches.",
            set_up_script: &[
                "SELECT set_config('test.hash', dolt_hashof('HEAD'), false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('HEAD', current_setting('test.hash'));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: 'HEAD' is an invalid branch name.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch(current_setting('test.hash'), 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "is an invalid branch name.", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('altBranch', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'altBranch', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "not a valid user branch name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'altBranch', current_setting('test.hash'));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "not a valid user branch name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'altBranch', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: 'HEAD' is not a valid branch name.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'altBranch', current_setting('test.hash'));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "is not a valid branch name.", message_contains: true, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: renaming default branch should update init.defaultbranch config",
            intent: "Renaming the default branch makes the new name the one new sessions start on.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', '-f', 'main', 'altmain');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust always runs as a server, and like Dolt's sql-server it refuses renaming the default branch without --force, so the port passes -f; Dolt skips this test (\"renames main branch which is not allowed with remote server\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("altmain")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: renaming non-default branch should not affect init.defaultbranch config",
            intent: "Renaming a branch other than the default leaves the session on the default branch.",
            set_up_script: &[
                "SELECT dolt_branch('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'feature', 'newfeature');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                            &[T("newfeature")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name = 'feature';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
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
            name: "branch: deleting an unmerged branch with a remote",
            intent: "dolt_branch -d judges a branch with an upstream against that upstream: it refuses one ahead and deletes ones even with or behind it.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b1');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b2');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "postgres/b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'new table') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "postgres/b1",
                    database: "postgres/b1",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "postgres/b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'new table') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('origin', 'b3');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'HEAD~');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/b3",
                    database: "postgres/b3",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "postgres/b3",
                    database: "postgres/b3",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch 'b1' is not fully merged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: dolt branch set upstream flag sets upstream",
            intent: "Creating a branch with --set-upstream-to sets its upstream.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('testUpstream', '--set-upstream-to', 'origin/main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT remote, branch FROM dolt_branches WHERE name = 'testUpstream';",
                    expected: Expected::Rows {
                        columns: &[Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T("origin"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("the CLI prints the branch it set up to track, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: can change upstream of existing branch with --set-upstream-to",
            intent: "--set-upstream-to on an existing branch changes its upstream.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');",
                "SELECT dolt_branch('br1', '--set-upstream-to', 'origin/main');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('--set-upstream', 'origin', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/other",
                    database: "postgres/other",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT remote, branch FROM dolt_branches WHERE name = 'br1';",
                    expected: Expected::Rows {
                        columns: &[Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T("origin"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--set-upstream-to', 'origin/other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT remote, branch FROM dolt_branches WHERE name = 'br1';",
                    expected: Expected::Rows {
                        columns: &[Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T("origin"), T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: can change upstream of existing branch with --set-upstream-to and current branch is assumed",
            intent: "--set-upstream-to without a branch name changes the current branch's upstream.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('--set-upstream', 'origin', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/other",
                    database: "postgres/other",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('--set-upstream-to', 'origin/other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT remote, branch FROM dolt_branches WHERE name = 'main';",
                    expected: Expected::Rows {
                        columns: &[Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T("origin"), T("other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: cannot set upstream of branches with invalid remote",
            intent: "--track and --set-upstream-to refuse a remote branch that doesn't exist.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--track', 'origin/invalid');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: 'origin/invalid'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('main', '--set-upstream-to', 'origin/invalid');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: 'origin/invalid'", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: cannot use both --track and --set-upstream-to",
            intent: "--set-upstream-to and --track can't be given together.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--set-upstream-to', 'origin/main', '--track', 'origin/main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: --set-upstream-to and --track are mutually exclusive options.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: --track sets upstream",
            intent: "Creating a branch with --track sets its upstream.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--track', 'origin/main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT remote, branch FROM dolt_branches WHERE name = 'br1';",
                    expected: Expected::Rows {
                        columns: &[Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T("origin"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("the CLI prints the branch it set up to track, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: can specify local branch with --track",
            intent: "--track can name a local branch as the upstream.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--track', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT remote, branch FROM dolt_branches WHERE name = 'br1';",
                    expected: Expected::Rows {
                        columns: &[Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(""), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("the CLI prints the branch it set up to track, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: --track presumes current branch without argument",
            intent: "--track without an upstream tracks the current branch.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--track');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT remote, branch FROM dolt_branches WHERE name = 'br1';",
                    expected: Expected::Rows {
                        columns: &[Column("remote", TEXT), Column("branch", TEXT)],
                        rows: &[
                            &[T(""), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("the CLI prints the branch it set up to track, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: --set-upstream-to works with starting point",
            intent: "--set-upstream-to with a starting point creates the branch at that commit.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "CREATE TABLE t (i int);",
                "SELECT dolt_commit('-Am', 'Created a table');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--set-upstream-to', 'origin/main', (SELECT commit_hash FROM dolt_log WHERE message = 'Initialize data repository'));",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM information_schema.tables WHERE table_schema = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/br1",
                    database: "postgres/br1",
                    dolt: Some(Dolt::MySqlOnly("dolt ls is a CLI command, so the port reads information_schema.tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: --set-upstream-to and --track presume HEAD starting point",
            intent: "--set-upstream-to and --track start a new branch at HEAD, not at the upstream.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT dolt_branch('br1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'A new commit') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/br1",
                    database: "postgres/br1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('--set-upstream', 'origin', 'br1:main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/br1",
                    database: "postgres/br1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('setUpstream', '--set-upstream-to', 'origin/main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT latest_commit_message FROM dolt_branches WHERE name = 'setUpstream';",
                    expected: Expected::Rows {
                        columns: &[Column("latest_commit_message", TEXT)],
                        rows: &[
                            &[T(r#"Initialize data repository"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('trackUpstream', '--track', 'origin/main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT latest_commit_message FROM dolt_branches WHERE name = 'trackUpstream';",
                    expected: Expected::Rows {
                        columns: &[Column("latest_commit_message", TEXT)],
                        rows: &[
                            &[T(r#"Initialize data repository"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: --set-upstream-to and --track can set HEAD starting point",
            intent: "--set-upstream-to with HEAD as the starting point starts the branch at HEAD.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT dolt_branch('br1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'A new commit') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/br1",
                    database: "postgres/br1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('--set-upstream', 'origin', 'br1:main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/br1",
                    database: "postgres/br1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('setUpstream', '--set-upstream-to', 'origin/main', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT latest_commit_message FROM dolt_branches WHERE name = 'setUpstream';",
                    expected: Expected::Rows {
                        columns: &[Column("latest_commit_message", TEXT)],
                        rows: &[
                            &[T(r#"Initialize data repository"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: --set-upstream-to and --track cannot set branch as its own upstream",
            intent: "A branch can't be its own upstream, and --track refuses a branch that doesn't exist.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('--set-upstream-to', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "\tnot setting 'main' as its own upstream", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--track', 'br1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: 'br1'", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: --set-upstream-to and --track cannot set relative commit as upstream",
            intent: "A relative commit can't be an upstream.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'Empty commit 1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--track', 'HEAD~1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: 'HEAD~1'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1', '--set-upstream-to', 'HEAD~1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: 'HEAD~1'", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: deleting a merged branch whose upstream was deleted on the remote",
            intent: "dolt_branch -d deletes a merged branch whose upstream a prune removed.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_push('origin', 'main');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('origin', ':b1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_fetch('-p');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_fetch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b1")],
                            &[T("main")],
                            &[T("remotes/origin/main")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch -a is a CLI command, so the port reads dolt_branches and dolt_remote_branches")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: deleting an unmerged branch whose upstream was deleted on the remote",
            intent: "dolt_branch -d refuses an unmerged branch whose upstream a prune removed as not fully merged, and -D deletes it.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_push('origin', 'main');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "postgres/b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'new table') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "postgres/b1",
                    database: "postgres/b1",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('origin', ':b1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_fetch('-p');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_fetch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch 'b1' is not fully merged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: deleting a branch whose upstream has a different name than the branch",
            intent: "dolt_branch -d deletes a branch whose upstream has another name.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_push('origin', 'main');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_push('-u', 'origin', 'b1:other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b1")],
                            &[T("main")],
                            &[T("remotes/origin/main")],
                            &[T("remotes/origin/other")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch -a is a CLI command, so the port reads dolt_branches and dolt_remote_branches")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: deleting a branch with an upstream works while the remote is unreachable",
            intent: "dolt_branch -d judges branches against their remote-tracking branches, so it works while the remote is gone.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_push('origin', 'main');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "postgres/b2",
                    database: "postgres/b2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'new table') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/b2",
                    database: "postgres/b2",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "postgres/b2",
                    database: "postgres/b2",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches UNION ALL SELECT name FROM dolt_remote_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b1")],
                            &[T("b2")],
                            &[T("main")],
                            &[T("remotes/origin/b1")],
                            &[T("remotes/origin/b2")],
                            &[T("remotes/origin/main")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::MySqlOnly("the bats test deletes the remote's directory, which a SQL session can't, so the port checks the same deletes with the remote in place, and dolt branch -a is a CLI command, so it reads dolt_branches and dolt_remote_branches")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch 'b2' is not fully merged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch: deleting an unmerged branch after its remote was removed",
            intent: "dolt_branch -d refuses an unmerged branch whose remote was removed, and -D deletes it.",
            set_up_script: &[
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_push('origin', 'main');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "postgres/b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'new table') IS NOT NULL AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "postgres/b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "postgres/b1",
                    database: "postgres/b1",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('remove', 'origin');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch 'b1' is not fully merged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt branch is a CLI command, so the port reads dolt_branches")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
