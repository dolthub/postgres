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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_rebase.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_rebase_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_rebase errors: basic errors",
            intent: "dolt_rebase refuses missing, malformed, and conflicting arguments, and --continue or --abort without a rebase in progress.",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "didn't identify any commits!", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "not enough args", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main1', 'main2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "rebase takes at most one positional argument.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--abrot');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: unknown option `abrot'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'doesnotexist');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: doesnotexist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase errors: working set not clean",
            intent: "dolt_rebase refuses to start while the working set has changes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO t VALUES (0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot start a rebase with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot start a rebase with uncommitted changes", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase errors: no database selected",
            intent: "dolt_rebase refuses to run without a current database.",
            set_up_script: &[
                "CREATE database temp;",
                "use temp;",
                "DROP database temp;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT CURRENT_SCHEMA();",
                    expected: Expected::Rows {
                        columns: &[Column("current_schema", NAME)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no database selected", ..E }),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a Postgres session always has a current database, so dolt_rebase never runs without one"),
            ..S
        },
        ScriptTest {
            name: "dolt_rebase errors: active merge, cherry-pick, or rebase",
            intent: "dolt_rebase refuses to start while a merge, cherry-pick, or rebase is in progress.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (0, 'nada');",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to start rebase while a merge is in progress – abort the current merge before proceeding", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: non-interactive rebase successful",
            intent: "A non-interactive dolt_rebase replays the branch's commits onto the upstream.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t on main');",
                "INSERT INTO t VALUES (42);",
                "SELECT dolt_commit('-am', 'main commit with pk = 42');",
                "SELECT dolt_branch('feature', 'HEAD~1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'feature commit 1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'feature commit 2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t WHERE pk = 42;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: edit action functionality test",
            intent: "An edit action pauses the rebase after its commit so it can be amended, and --continue finishes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t on main');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'feature commit 1');",
                "SELECT * FROM dolt_rebase('--interactive', 'main');",
                "UPDATE dolt_rebase SET action = 'edit' WHERE rebase_order = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT action FROM dolt_rebase WHERE rebase_order = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("action", DOLT_REBASE_ACTION)],
                        rows: &[
                            &[T("edit")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
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
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: multiple edit actions",
            intent: "A rebase with several edit actions pauses at each, and --continue moves through them.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t on main');",
                "SELECT dolt_branch('feature2');",
                "SELECT * FROM dolt_checkout('feature2');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'commit 1');",
                "INSERT INTO t VALUES (20);",
                "SELECT dolt_commit('-am', 'commit 2');",
                "SELECT * FROM dolt_rebase('--interactive', 'main');",
                "UPDATE dolt_rebase SET action = 'edit';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
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
                    query: "SELECT * FROM dolt_rebase('--continue');",
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
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/feature2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: edit action followed by conflict",
            intent: "After an edit pause, a later commit's conflict stops the rebase until it is resolved.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (pk int PRIMARY KEY, val varchar(100));",
                "INSERT INTO t VALUES (1, 'original1'), (2, 'original2'), (3, 'original3');",
                "SELECT dolt_commit('-Am', 'initial table with data');",
                "SELECT dolt_branch('feature');",
                "UPDATE t SET val = 'main_updated_row2' WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'main updates row 2');",
                "SELECT * FROM dolt_checkout('feature');",
                "UPDATE t SET val = 'feature_updated_row1' WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'feature updates row 1');",
                "UPDATE t SET val = 'feature_updated_row2' WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'feature updates row 2');",
                "SELECT * FROM dolt_rebase('--interactive', 'main');",
                "UPDATE dolt_rebase SET action = 'edit' WHERE rebase_order = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT rebase_order, action, commit_message FROM dolt_rebase ORDER BY rebase_order;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("edit"), T("feature updates row 1")],
                            &[T("2.00"), T("pick"), T("feature updates row 2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
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
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", VARCHAR)],
                        rows: &[
                            &[T("1"), T("feature_updated_row1")],
                            &[T("2"), T("main_updated_row2")],
                            &[T("3"), T("original3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val = 'edited_during_rebase_row3' WHERE pk = 3;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'feature updates row 1 and 3 (edited)');",
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
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val = 'resolved_conflict_row2' WHERE pk = 2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", VARCHAR)],
                        rows: &[
                            &[T("1"), T("feature_updated_row1")],
                            &[T("2"), T("resolved_conflict_row2")],
                            &[T("3"), T("edited_during_rebase_row3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("feature updates row 2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase errors: rebase working branch already exists",
            intent: "dolt_rebase refuses when its working branch already exists.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('dolt_rebase_branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'inserting row 10');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: A branch named 'dolt_rebase_branch1' already exists.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase errors: invalid rebase plans",
            intent: "dolt_rebase --continue refuses plans with unknown actions, missing commits, or a squash or fixup first.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'inserting row 10');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=1.0 WHERE rebase_order=2.0;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"dolt_rebase_pkey\"", detail: "Key (rebase_order)=(1.00) already exists.", ..E }),
                    dolt: Some(Dolt::Differs("the rebase plan is the dolt_rebase view over the rebase state, whose writes keep rebase_order a key as a Postgres unique index does (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action='squash';",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid rebase plan: squash and fixup actions must appear after a pick or reword action", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action='drop' WHERE rebase_order=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid rebase plan: squash and fixup actions must appear after a pick or reword action", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action='pick', commit_hash='doesnotexist' WHERE rebase_order=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid commit hash: doesnotexist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET commit_hash='0123456789abcdef0123456789abcdef0123456789abcdef0120' WHERE rebase_order=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to resolve commit hash 0123456789abcdef0123456789abcdef0123456789abcdef0120: target commit not found", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's commit hashes are 52 characters, so a hash of no commit is written at that length")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase errors: unresolved conflicts",
            intent: "dolt_rebase --continue refuses while conflicts are unresolved.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'inserting row 1 on branch1');",
                "UPDATE t SET c1='uno' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1 on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_rebase WHERE rebase_order=1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("2.00"), T("pick"), Any, T("updating row 1 on branch1")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "conflicts detected in tables", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase errors: uncommitted changes before first --continue",
            intent: "dolt_rebase --continue refuses uncommitted changes made before the plan starts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'inserting row 1 on branch1');",
                "UPDATE t SET c1='uno' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1 on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (100, 'hundo');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot start a rebase with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase errors: data conflicts when @@autocommit is enabled",
            intent: "Outside a transaction block, a rebase that hits a data conflict fails and aborts.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'inserting row 1 on branch1');",
                "UPDATE t SET c1='uno' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1 on branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=3 WHERE rebase_order=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("2.00"), T("pick"), Any, T("updating row 1 on branch1")],
                            &[T("3.00"), T("pick"), Any, T("inserting row 1 on branch1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[&[T("1"), T("data conflicts from rebase, but session settings do not allow preserving conflicts, so they cannot be resolved. The rebase has been aborted. Use an explicit transaction (BEGIN ... COMMIT) or set dolt_allow_commit_conflicts to on and try the rebase again to resolve the conflicts.")]],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Doltgrust returns a rebase's stop at a conflict, and the abort a conflict forces, as a row with status 1 and Dolt's message instead of an error, since an error would roll back the stop or the abort (Appendix C); the message also names the setting as Postgres sets it, with no @@")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("branch1")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"dolt_rebase\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust keeps a rebase's plan in its rebase state and shows it through the dolt_rebase view, so reading it without a rebase fails as Postgres fails for a missing relation (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
            name: "dolt_rebase: rebased commit becomes empty; --empty not specified",
            intent: "Without --empty, a commit that becomes empty when rebased is dropped.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0 on branch1');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'inserting row 10 on branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
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
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 10 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: rebased commit becomes empty; --empty=keep",
            intent: "With --empty=keep, a commit that becomes empty when rebased is kept.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0 on branch1');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'inserting row 10 on branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', '--empty', 'keep', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
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
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 10 on branch1")],
                            &[T("inserting row 0 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: rebased commit becomes empty; --empty=drop",
            intent: "With --empty=drop, a commit that becomes empty when rebased is dropped.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0 on branch1');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'inserting row 10 on branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', '--empty', 'drop', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
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
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 10 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: no commits to rebase",
            intent: "dolt_rebase refuses when the branch has no commits beyond the upstream.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'inserting row 10');",
                "INSERT INTO t VALUES (100);",
                "SELECT dolt_commit('-am', 'inserting row 100');",
                "INSERT INTO t VALUES (1000);",
                "SELECT dolt_commit('-am', 'inserting row 1000');",
                "INSERT INTO t VALUES (10000);",
                "SELECT dolt_commit('-am', 'inserting row 10000');",
                "INSERT INTO t VALUES (100000);",
                "SELECT dolt_commit('-am', 'inserting row 100000');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "didn't identify any commits!", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_branches WHERE name='dolt_rebase_branch1';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("hash", TEXT), Column("latest_committer", TEXT), Column("latest_committer_email", TEXT), Column("latest_commit_date", TIMESTAMPTZ), Column("latest_commit_message", TEXT), Column("remote", TEXT), Column("branch", TEXT), Column("dirty", BOOL), Column("latest_author", TEXT), Column("latest_author_email", TEXT), Column("latest_author_date", TIMESTAMPTZ)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: abort properly cleans up",
            intent: "dolt_rebase --abort returns to the original branch and removes the working branch.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'inserting row 10');",
                "INSERT INTO t VALUES (100);",
                "SELECT dolt_commit('-am', 'inserting row 100');",
                "INSERT INTO t VALUES (1000);",
                "SELECT dolt_commit('-am', 'inserting row 1000');",
                "INSERT INTO t VALUES (10000);",
                "SELECT dolt_commit('-am', 'inserting row 10000');",
                "INSERT INTO t VALUES (100000);",
                "SELECT dolt_commit('-am', 'inserting row 100000');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
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
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Interactive rebase aborted")],
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
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                            &[T("branch1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: rebase plan using every action",
            intent: "A plan using pick, drop, squash, fixup, and reword rewrites the history accordingly.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'inserting row 10');",
                "INSERT INTO t VALUES (100);",
                "SELECT dolt_commit('-am', 'inserting row 100');",
                "INSERT INTO t VALUES (1000);",
                "SELECT dolt_commit('-am', 'inserting row 1000');",
                "INSERT INTO t VALUES (10000);",
                "SELECT dolt_commit('-am', 'inserting row 10000');",
                "INSERT INTO t VALUES (100000);",
                "SELECT dolt_commit('-am', 'inserting row 100000');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("inserting row 1")],
                            &[T("2.00"), T("pick"), Any, T("inserting row 10")],
                            &[T("3.00"), T("pick"), Any, T("inserting row 100")],
                            &[T("4.00"), T("pick"), Any, T("inserting row 1000")],
                            &[T("5.00"), T("pick"), Any, T("inserting row 10000")],
                            &[T("6.00"), T("pick"), Any, T("inserting row 100000")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=6.1 WHERE rebase_order=6;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action='squash' WHERE rebase_order IN (2, 3);",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action='drop' WHERE rebase_order = 4;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action='reword', commit_message='reworded!' WHERE rebase_order = 5;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action='fixup' WHERE rebase_order = 6.10;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("inserting row 1")],
                            &[T("2.00"), T("squash"), Any, T("inserting row 10")],
                            &[T("3.00"), T("squash"), Any, T("inserting row 100")],
                            &[T("4.00"), T("drop"), Any, T("inserting row 1000")],
                            &[T("5.00"), T("reword"), Any, T("reworded!")],
                            &[T("6.10"), T("fixup"), Any, T("inserting row 100000")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no rebase in progress", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"dolt_rebase\" does not exist", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust keeps a rebase's plan in its rebase state and shows it through the dolt_rebase view, so reading it without a rebase fails as Postgres fails for a missing relation (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                            &[T("branch1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log ORDER BY date DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("reworded!")],
                            &[T("inserting row 1\n\ninserting row 10\n\ninserting row 100")],
                            &[T("inserting row 0")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("10")],
                            &[T("100")],
                            &[T("10000")],
                            &[T("100000")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: negative rebase order",
            intent: "A plan's commits may be reordered with negative rebase orders.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'inserting row 1 on branch1');",
                "UPDATE t SET c1='uno' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1 on branch1');",
                "UPDATE t SET c1='ein' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1, again, on branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=rebase_order-12.34;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("-11.34"), T("pick"), Any, T("inserting row 1 on branch1")],
                            &[T("-10.34"), T("pick"), Any, T("updating row 1 on branch1")],
                            &[T("-9.34"), T("pick"), Any, T("updating row 1, again, on branch1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("1"), T("ein")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("updating row 1, again, on branch1")],
                            &[T("updating row 1 on branch1")],
                            &[T("inserting row 1 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: data conflicts with pick",
            intent: "A picked commit's data conflict stops the rebase until it is resolved and continued.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'inserting row 1 on branch1');",
                "UPDATE t SET c1='uno' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1 on branch1');",
                "UPDATE t SET c1='ein' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1, again, on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("inserting row 1 on branch1")],
                            &[T("2.00"), T("pick"), Any, T("updating row 1 on branch1")],
                            &[T("3.00"), T("pick"), Any, T("updating row 1, again, on branch1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=3.5 WHERE rebase_order=1;",
                    expected: Expected::Tag("UPDATE 1"),
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), Null, Null, T("removed"), T("1"), T("uno"), T("modified")],
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
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("1"), T("uno")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot continue a rebase with unstaged changes. Use dolt_add() to stage tables and then continue the rebase", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("1"), T("ein")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("updating row 1, again, on branch1")],
                            &[T("updating row 1 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("1"), T("ein"), T("added"), T("1"), T("one"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'OVERRIDDEN COMMIT MESSAGE');",
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
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("OVERRIDDEN COMMIT MESSAGE")],
                            &[T("updating row 1, again, on branch1")],
                            &[T("updating row 1 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: data conflicts with squash",
            intent: "A squashed commit's data conflict stops the rebase until it is resolved and continued.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'inserting row 1 on branch1');",
                "UPDATE t SET c1='uno' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1 on branch1');",
                "UPDATE t SET c1='ein' WHERE pk=1;",
                "SELECT dolt_commit('-am', 'updating row 1, again, on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("inserting row 1 on branch1")],
                            &[T("2.00"), T("pick"), Any, T("updating row 1 on branch1")],
                            &[T("3.00"), T("pick"), Any, T("updating row 1, again, on branch1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=3.5 WHERE rebase_order=2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET action='squash' WHERE rebase_order=3;",
                    expected: Expected::Tag("UPDATE 1"),
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("uno"), T("1"), T("one"), T("modified"), T("1"), T("ein"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 1 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("1"), T("ein")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot continue a rebase with unstaged changes. Use dolt_add() to stage tables and then continue the rebase", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("1"), T("ein")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 1 on branch1\n\nupdating row 1, again, on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("1"), T("ein"), T("modified"), T("1"), T("uno"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'OVERRIDDEN COMMIT MESSAGE');",
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
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("1"), T("uno")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("OVERRIDDEN COMMIT MESSAGE")],
                            &[T("inserting row 1 on branch1\n\nupdating row 1, again, on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: data conflicts with fixup",
            intent: "A fixup commit's data conflict stops the rebase until it is resolved and continued.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "INSERT INTO t VALUES (-1, 'negative');",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET c1='-1' WHERE pk=-1;",
                "SELECT dolt_commit('-am', 'updating row -1 on branch1');",
                "DELETE FROM t WHERE c1 = '-1';",
                "SELECT dolt_commit('-am', 'deleting -1 on branch1');",
                "INSERT INTO t VALUES (999, 'nines');",
                "SELECT dolt_commit('-am', 'inserting row 999 on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("updating row -1 on branch1")],
                            &[T("2.00"), T("pick"), Any, T("deleting -1 on branch1")],
                            &[T("3.00"), T("pick"), Any, T("inserting row 999 on branch1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=3.5, action='fixup' WHERE rebase_order=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("2.00"), T("pick"), Any, T("deleting -1 on branch1")],
                            &[T("3.00"), T("pick"), Any, T("inserting row 999 on branch1")],
                            &[T("3.50"), T("fixup"), Any, T("updating row -1 on branch1")],
                        ],
                        tag: "SELECT 3",
                    },
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("-1"), T("-1"), T("-1"), T("negative"), T("modified"), Null, Null, T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot continue a rebase with unstaged changes. Use dolt_add() to stage tables and then continue the rebase", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("999"), T("nines")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 999 on branch1")],
                            &[T("deleting -1 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("-1"), T("negative"), Null, Null, T("removed"), T("-1"), T("-1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("-1"), T("-1")],
                            &[T("0"), T("zero")],
                            &[T("999"), T("nines")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 999 on branch1")],
                            &[T("deleting -1 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: data conflicts with reword",
            intent: "A reworded commit's data conflict stops the rebase until it is resolved and continued.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "INSERT INTO t VALUES (-1, 'negative');",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0, 'zero');",
                "SELECT dolt_commit('-am', 'inserting row 0 on main');",
                "SELECT * FROM dolt_checkout('branch1');",
                "UPDATE t SET c1='-1' WHERE pk=-1;",
                "SELECT dolt_commit('-am', 'updating row -1 on branch1');",
                "DELETE FROM t WHERE c1 = '-1';",
                "SELECT dolt_commit('-am', 'deleting -1 on branch1');",
                "INSERT INTO t VALUES (999, 'nines');",
                "SELECT dolt_commit('-am', 'inserting row 999 on branch1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("updating row -1 on branch1")],
                            &[T("2.00"), T("pick"), Any, T("deleting -1 on branch1")],
                            &[T("3.00"), T("pick"), Any, T("inserting row 999 on branch1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=3.5, action='reword', commit_message='reworded message!' WHERE rebase_order=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("2.00"), T("pick"), Any, T("deleting -1 on branch1")],
                            &[T("3.00"), T("pick"), Any, T("inserting row 999 on branch1")],
                            &[T("3.50"), T("reword"), Any, T("reworded message!")],
                        ],
                        tag: "SELECT 3",
                    },
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("dolt_rebase_branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("-1"), T("-1"), T("-1"), T("negative"), T("modified"), Null, Null, T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot continue a rebase with unstaged changes. Use dolt_add() to stage tables and then continue the rebase", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("zero")],
                            &[T("999"), T("nines")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 999 on branch1")],
                            &[T("deleting -1 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("-1"), T("negative"), Null, Null, T("removed"), T("-1"), T("-1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("-1"), T("-1")],
                            &[T("0"), T("zero")],
                            &[T("999"), T("nines")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("reworded message!")],
                            &[T("inserting row 999 on branch1")],
                            &[T("deleting -1 on branch1")],
                            &[T("inserting row 0 on main")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: schema conflicts",
            intent: "A schema conflict during a rebase aborts it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "ALTER TABLE t ADD COLUMN c1 varchar(100) NOT NULL DEFAULT '';",
                "SELECT dolt_commit('-am', 'adding column c1');",
                "ALTER TABLE t ALTER COLUMN c1 TYPE varchar(100), ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 DROP DEFAULT; COMMENT ON COLUMN t.c1 IS 'foo';",
                "SELECT dolt_commit('-am', 'altering column c1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("inserting row 1")],
                            &[T("2.00"), T("pick"), Any, T("adding column c1")],
                            &[T("3.00"), T("pick"), Any, T("altering column c1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_rebase SET rebase_order=3.1 WHERE rebase_order=2;",
                    expected: Expected::Tag("UPDATE 1"),
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("base_schema", TEXT), Column("our_schema", TEXT), Column("their_schema", TEXT), Column("description", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: non-standard plan changes",
            intent: "Plan rows may be inserted, deleted, and renumbered before --continue.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch2');",
                "INSERT INTO t VALUES (999);",
                "SELECT dolt_commit('-am', 'inserting row 999');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'inserting row 2');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'inserting row 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("inserting row 1")],
                            &[T("2.00"), T("pick"), Any, T("inserting row 2")],
                            &[T("3.00"), T("pick"), Any, T("inserting row 3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_rebase WHERE rebase_order > 1;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_rebase VALUES (2.12, 'pick', dolt_hashof('branch2'), 'inserting row 0');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 999")],
                            &[T("inserting row 1")],
                            &[T("inserting row 0")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("999")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: merge commits",
            intent: "dolt_rebase leaves out merge commits from its plan.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (0);",
                "SELECT dolt_commit('-am', 'inserting row 0');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'inserting row 2');",
                "SELECT * FROM dolt_merge('main');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'inserting row 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 3")],
                            &[T("Merge branch 'main' into branch1")],
                            &[T("inserting row 2")],
                            &[T("inserting row 1")],
                            &[T("inserting row 0")],
                            &[T("creating table t")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase ORDER BY rebase_order;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("inserting row 1")],
                            &[T("2.00"), T("pick"), Any, T("inserting row 2")],
                            &[T("3.00"), T("pick"), Any, T("inserting row 3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("inserting row 3")],
                            &[T("inserting row 2")],
                            &[T("inserting row 1")],
                            &[T("inserting row 0")],
                            &[T("creating table t")],
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
            name: "dolt_rebase: handles multi-line commit messages",
            intent: "Rebasing keeps multi-line commit messages.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'empty commit 1');",
                "SELECT dolt_commit('--allow-empty', '-m', 'empty \ncommit \n2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("empty \ncommit \n2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', '--empty=keep', 'HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_main; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("pick"), Any, T("empty \ncommit \n2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("empty \ncommit \n2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: comprehensive edit and conflict workflow",
            intent: "A rebase with edits and conflicts runs to the end through resolutions and --continue.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t1 (pk int PRIMARY KEY, val int);",
                "INSERT INTO t1 VALUES (1, 1), (5, 5);",
                "SELECT dolt_commit('-Am', 'main commit 1');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_checkout('b1');",
                "UPDATE t1 SET val = 11 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'b1 commit 1 - to edit');",
                "INSERT INTO t1 VALUES (10, 100);",
                "SELECT dolt_commit('-am', 'b1 commit 2 - will conflict');",
                "INSERT INTO t1 VALUES (20, 200);",
                "SELECT dolt_commit('-am', 'b1 commit 3 - clean apply');",
                "INSERT INTO t1 VALUES (30, 300);",
                "SELECT dolt_commit('-am', 'b1 commit 4 - edit at end');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t1 VALUES (10, 999);",
                "SELECT dolt_commit('-am', 'main conflicting data');",
                "SELECT * FROM dolt_checkout('b1');",
                "SELECT * FROM dolt_rebase('-i', 'main');",
                "UPDATE dolt_rebase SET action = 'edit' WHERE rebase_order = 1;",
                "UPDATE dolt_rebase SET action = 'edit' WHERE rebase_order = 4;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT rebase_order, action, commit_message FROM dolt_rebase ORDER BY rebase_order;",
                    expected: Expected::Rows {
                        columns: &[Column("rebase_order", NUMERIC), Column("action", DOLT_REBASE_ACTION), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("1.00"), T("edit"), T("b1 commit 1 - to edit")],
                            &[T("2.00"), T("pick"), T("b1 commit 2 - will conflict")],
                            &[T("3.00"), T("pick"), T("b1 commit 3 - clean apply")],
                            &[T("4.00"), T("edit"), T("b1 commit 4 - edit at end")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
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
                    query: "SELECT pk, val FROM t1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("5"), T("5")],
                            &[T("10"), T("999")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 SET val = 55 WHERE pk = 5;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--amend', '-m', 'b1 commit 1 - to edit (modified)');",
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
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
                    query: "SELECT our_pk, our_val, their_pk, their_val FROM dolt_conflicts_t1;",
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT4), Column("our_val", INT4), Column("their_pk", INT4), Column("their_val", INT4)],
                        rows: &[
                            &[T("10"), T("999"), T("10"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 SET val = 200 WHERE pk = 10;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
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
                    query: "SELECT pk, val FROM t1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("5"), T("55")],
                            &[T("10"), T("200")],
                            &[T("20"), T("200")],
                            &[T("30"), T("300")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (42, 24);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b1 commit 5 - additional commit during edit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/b1")],
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
                            &[T("b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("b1 commit 5 - additional commit during edit")],
                            &[T("b1 commit 4 - edit at end")],
                            &[T("b1 commit 3 - clean apply")],
                            &[T("b1 commit 2 - will conflict")],
                            &[T("b1 commit 1 - to edit (modified)")],
                            &[T("main conflicting data")],
                            &[T("main commit 1")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase: ignored tables",
            intent: "dolt_rebase leaves ignored tables in the working set.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "INSERT INTO dolt_ignore VALUES ('ignored_table', TRUE);",
                "SELECT dolt_commit('-Am', 'Initial table');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'More data');",
                "INSERT INTO dolt_ignore (pattern, ignored) VALUES ('ignored_%', TRUE);",
                "SELECT dolt_commit('-am', 'Ignore rules');",
                "CREATE TABLE ignored_table (id int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'HEAD~2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_main; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_rebase('--continue') preserves ignored tables in the working set",
            intent: "dolt_rebase --continue keeps ignored tables in the working set.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'create t');",
                "INSERT INTO dolt_ignore VALUES ('private_data', TRUE);",
                "SELECT dolt_commit('-Am', 'add ignore rule');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'insert 1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'insert 2');",
                "CREATE TABLE private_data (pk int PRIMARY KEY, secret varchar(100));",
                "INSERT INTO private_data VALUES (1, 'secret');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'HEAD~2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_main; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/main")],
                        ],
                        tag: "SELECT 1",
                    },
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
    ]);
}

#[test]
fn test_dolt_rebase_multi_session_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_rebase errors: branch HEAD changed during rebase",
            intent: "dolt_rebase --continue fails when the branch's head moved during the rebase.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'inserting row 1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'inserting row 2');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'inserting row 3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('-i', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1000);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'inserting row 1000');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "rebase aborted due to changes in branch branch1", ..E }),
                    dolt: Some(Dolt::MySqlOnly("Error 1105 (HY000) is the MySQL client's prefix of a server error")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
