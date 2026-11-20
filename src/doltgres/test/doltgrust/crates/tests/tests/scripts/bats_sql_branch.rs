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

//! Dolt's integration-tests/bats/sql-branch.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_branch() {
    run_scripts(&[
        ScriptTest {
            name: "sql-branch: CALL DOLT_BRANCH works",
            intent: "dolt_branch makes a branch without checking it out.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE name = 'new-branch';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('new-branch');",
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
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                            &[T("new-branch")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-branch: CALL DOLT_BRANCH error cases",
            intent: "dolt_branch refuses a name that is taken and an empty one.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_branch('existing_branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('existing_branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'existing_branch' already exists.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: cannot branch empty string", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-branch: CALL DOLT_BRANCH -c copies not current branch and stays on current branch",
            intent: "dolt_branch -c copies another branch, with its commits, and leaves the session on its branch.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', '0, 1, and 2 in test table');",
                "SELECT * FROM dolt_checkout('-b', 'original');",
                "INSERT INTO test VALUES (4);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'add 4 in original');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'original', 'copy');",
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
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('copy');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'copy'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-branch: CALL DOLT_BRANCH -c throws error on error cases",
            intent: "dolt_branch -c refuses empty names, a source that doesn't exist, and a target that does.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_branch('existing_branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', '', 'copy');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: cannot branch empty string", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'main', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: cannot branch empty string", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("existing_branch")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'original', 'copy');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'original' not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-c', 'main', 'existing_branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'existing_branch' already exists.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-branch: CALL DOLT_BRANCH works as insert into dolt_branches table",
            intent: "A branch dolt_branch makes shows in dolt_branches at its start point's hash.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', '1, 2, and 3 in test table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('feature-branch');",
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
                    query: "SELECT (SELECT hash FROM dolt_branches WHERE name = 'feature-branch') = (SELECT hash FROM dolt_branches WHERE name = 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-branch: CALL DOLT_BRANCH to rename and delete",
            intent: "dolt_branch -m renames and -d deletes a merged branch, -d refuses one with unmerged commits, and -D deletes it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', '1, 2, and 3 in test table');",
                "SELECT dolt_branch('new_branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'new_branch', 'changed');",
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
                    query: "SELECT dolt_branch('-d', 'changed');",
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
                    query: "SELECT dolt_branch('branch_with_unpushed_commit');",
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
                    query: "SELECT * FROM dolt_checkout('branch_with_unpushed_commit');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch_with_unpushed_commit'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-am', 'empty commit');",
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
                    query: "SELECT dolt_branch('-d', 'branch_with_unpushed_commit');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "not fully merged", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'branch_with_unpushed_commit');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
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
            name: "sql-branch: CALL DOLT_BRANCH -d error cases",
            intent: "Deleting the checked-out default branch is refused.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', '1, 2, and 3 in test table');",
                "SELECT dolt_branch('new_branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to delete branch 'main', because it is the default branch", message_contains: true, ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust always runs as Dolt's sql-server does, which refuses to delete the default branch before it checks the checked-out one (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-branch: CALL DOLT_BRANCH -d -r to remove remote branch",
            intent: "dolt_branch -D -r deletes a remote-tracking branch.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_remote('add', 'origin', 'file://{NEWDIR:remote}');",
                "SELECT * FROM dolt_push('--set-upstream', 'origin', 'b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', '-r', 'origin/b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
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
            name: "sql-branch: CALL DOLT_BRANCH -m on session active branch (dolt sql)",
            intent: "Renaming the session's own branch keeps the session on it under its new name.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'other'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'other', 'newOther');",
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("newOther")],
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
                            &[T("newOther")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
