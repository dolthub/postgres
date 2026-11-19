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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_server_tests.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_branch_multi_session_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "Test multi-session behavior for deleting branches",
            intent: "A branch another session has checked out is deleted only with force, and plainly once that session leaves it.",
            set_up_script: &[
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "SELECT dolt_branch('branch3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
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
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unsafe to delete or rename branches in use in other sessions; use --force to force the change", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch2'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'branch2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unsafe to delete or rename branches in use in other sessions; use --force to force the change", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-df', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'branch3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test multi-session behavior for renaming branches",
            intent: "A branch another session has checked out is renamed only with force.",
            set_up_script: &[
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
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
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'branch1', 'movedBranch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unsafe to delete or rename branches in use in other sessions; use --force to force the change", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-mf', 'branch1', 'movedBranch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'branch2', 'movedBranch2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test branch deletion when clients are using a branch-qualified database",
            intent: "A branch another session logged in to is deleted only with force, and the session that deletes it keeps working.",
            set_up_script: &[
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_database(), active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("postgres"), T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::Differs("MySQL's USE of a branch-qualified database is a login to it, since a Postgres connection's database never changes; and current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database(), active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("postgres"), T("branch2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    database: "postgres/branch2",
                    dolt: Some(Dolt::Differs("MySQL's USE of a branch-qualified database is a login to it, since a Postgres connection's database never changes; and current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'branch2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unsafe to delete or rename branches in use in other sessions; use --force to force the change", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-df', 'branch2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database(), active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("postgres"), T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('branch3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test branch renaming when clients are using a branch-qualified database",
            intent: "A branch another session logged in to is renamed only with force, and the session that renames it keeps working.",
            set_up_script: &[
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_database(), active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("postgres"), T("branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    database: "postgres/branch1",
                    dolt: Some(Dolt::Differs("MySQL's USE of a branch-qualified database is a login to it, since a Postgres connection's database never changes; and current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database(), active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("postgres"), T("branch2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    database: "postgres/branch2",
                    dolt: Some(Dolt::Differs("MySQL's USE of a branch-qualified database is a login to it, since a Postgres connection's database never changes; and current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-m', 'branch2', 'newName');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unsafe to delete or rename branches in use in other sessions; use --force to force the change", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-mf', 'branch2', 'newName');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's, and lists the revision databases the session has used, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('branch3');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test multi-session behavior for force deleting active branch with autocommit on",
            intent: "Force-deleting the branch another session has checked out leaves that session unable to use it until it checks main out.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
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
                    client: "a",
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("branch1")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "branch1", message_contains: true, ..E }),
                    client: "a",
                    skip: Some("a session on a force-deleted branch gets the hint Use dolt_checkout to switch to another branch., which the expectation leaves out"),
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
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt refuses to check out another branch from a session whose branch was force-deleted, leaving it only USE, which Postgres has no form of, so the checkout works")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Already on branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE dolt/main is a checkout of main, since a Postgres connection's database never changes")),
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
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test multi-session behavior for force deleting active branch with autocommit off",
            intent: "Force-deleting the branch another session has checked out in an open transaction leaves that session unable to use it until it checks main out.",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'branch1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
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
                    client: "a",
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
                    client: "b",
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
                    client: "b",
                    dolt: Some(Dolt::Differs("A branch made in an open transaction is unseen by other sessions until the transaction commits, by Appendix C's ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block, so branch1 is never deleted and the session that made it keeps it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-D', 'branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found", ..E }),
                    client: "b",
                    dolt: Some(Dolt::Differs("A branch made in an open transaction is unseen by other sessions until the transaction commits, by Appendix C's ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block, so branch1 is never deleted and the session that made it keeps it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "a",
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
                    client: "a",
                    dolt: Some(Dolt::Differs("A branch made in an open transaction is unseen by other sessions until the transaction commits, by Appendix C's ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block, so branch1 is never deleted and the session that made it keeps it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "a",
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
                    client: "a",
                    dolt: Some(Dolt::Differs("A branch made in an open transaction is unseen by other sessions until the transaction commits, by Appendix C's ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block, so branch1 is never deleted and the session that made it keeps it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Already on branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("A branch made in an open transaction is unseen by other sessions until the transaction commits, by Appendix C's ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block, so branch1 is never deleted and the session that made it keeps it")),
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
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_drop_database_multi_session_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "Test multi-session behavior for dropping databases",
            intent: "Dropping a database two sessions are connected to leaves both with correct results.",
            set_up_script: &[
                "CREATE DATABASE db01;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t01 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    database: "db01",
                    dolt: Some(Dolt::MySqlOnly("MySQL's setup writes db01.t01 from another database, where Postgres writes a database only through a connection to it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t01 VALUES (101), (202), (303);",
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("MySQL's setup writes db01.t01 from another database, where Postgres writes a database only through a connection to it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    database: "db01",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE of another database is a login to it, since a Postgres connection's database never changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE db01;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: "cannot drop the currently open database", ..E }),
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database() IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname LIKE 'db01';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("db01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE db01;",
                    expected: Expected::Error(Diagnostic { code: "42P04", message: "database \"db01\" already exists", ..E }),
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("db01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test multi-session behavior for dropping databases with a revision db",
            intent: "Dropping a database one session uses and another uses a branch of leaves both with correct results.",
            set_up_script: &[
                "CREATE DATABASE db01;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t01 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    database: "db01",
                    dolt: Some(Dolt::MySqlOnly("MySQL's setup writes db01.t01 from another database, where Postgres writes a database only through a connection to it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t01 VALUES (101), (202), (303);",
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("MySQL's setup writes db01.t01 from another database, where Postgres writes a database only through a connection to it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'commit on main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("MySQL's setup writes db01.t01 from another database, where Postgres writes a database only through a connection to it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("MySQL's setup writes db01.t01 from another database, where Postgres writes a database only through a connection to it, and branches with dolt_branch so that the session stays on main")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t01 VALUES (1001), (2002), (3003);",
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "b",
                    database: "db01/branch1",
                    dolt: Some(Dolt::MySqlOnly("MySQL's setup writes db01.t01 from another database, where Postgres writes a database only through a connection to it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'commit on branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::MySqlOnly("MySQL's setup writes db01.t01 from another database, where Postgres writes a database only through a connection to it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE db01;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: "cannot drop the currently open database", ..E }),
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database() IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname LIKE 'db01';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("db01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE db01;",
                    expected: Expected::Error(Diagnostic { code: "42P04", message: "database \"db01\" already exists", ..E }),
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database();",
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T("db01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("current_database() names the database as pg_database lists it, not its revision, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t01")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to, where Dolt drops it under its sessions, so the rest of the script sees db01 unchanged")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_persist_variable_tests() {
    run_scripts(&[
        ScriptTest {
            name: "set persisted variables with on and off",
            intent: "Settings written with ALTER SYSTEM reach new sessions.",
            set_up_script: &[
                "ALTER SYSTEM SET dolt_skip_replication_errors = ON;",
                "ALTER SYSTEM SET dolt_read_replica_force_pull = off;",
                "SELECT pg_reload_conf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_setting('dolt_skip_replication_errors');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "fresh",
                    dolt: Some(Dolt::Differs("Dolt's @@persist setting survives a server restart, which a script can't make, so a new session reads the setting ALTER SYSTEM wrote, and Dolt's next script, which reads it after the restart, is part of this one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('dolt_read_replica_force_pull');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "fresh",
                    dolt: Some(Dolt::Differs("Dolt's @@persist setting survives a server restart, which a script can't make, so a new session reads the setting ALTER SYSTEM wrote, and Dolt's next script, which reads it after the restart, is part of this one")),
                    ..A
                },
            ],
            skip: Some("Stage 3: Dolt's read replica settings (dolt_skip_replication_errors, dolt_read_replica_force_pull)"),
            ..S
        },
        ScriptTest {
            name: "set persisted variables with 1 and 0",
            intent: "Settings written with ALTER SYSTEM reach new sessions.",
            set_up_script: &[
                "ALTER SYSTEM SET dolt_skip_replication_errors = 0;",
                "ALTER SYSTEM SET dolt_read_replica_force_pull = 1;",
                "SELECT pg_reload_conf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_setting('dolt_skip_replication_errors');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "fresh",
                    dolt: Some(Dolt::Differs("Dolt's @@persist setting survives a server restart, which a script can't make, so a new session reads the setting ALTER SYSTEM wrote, and Dolt's next script, which reads it after the restart, is part of this one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('dolt_read_replica_force_pull');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "fresh",
                    dolt: Some(Dolt::Differs("Dolt's @@persist setting survives a server restart, which a script can't make, so a new session reads the setting ALTER SYSTEM wrote, and Dolt's next script, which reads it after the restart, is part of this one")),
                    ..A
                },
            ],
            skip: Some("Stage 3: Dolt's read replica settings (dolt_skip_replication_errors, dolt_read_replica_force_pull)"),
            ..S
        },
    ]);
}
