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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_stash.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_stash_tests() {
    run_scripts(&[
        ScriptTest {
            name: "DOLT_STASH() subcommands error on empty space.",
            intent: "dolt_stash subcommands refuse an empty working set.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "No local changes to save", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "No local changes to save", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "No stash entries found.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('drop', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "No stash entries found.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('clear','myStash');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_stash", INT8)],
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
            name: "Simple push and pop with DOLT_STASH()",
            intent: "dolt_stash push saves and clears the working changes, and pop restores them.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_stash('push', 'myStash');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
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
            name: "Clearing stash removes all entries in stash list",
            intent: "dolt_stash clear removes every stash.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A','-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_stash('push', 'stash1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_stash('push', 'stash2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_stash('push', 'stash2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash1"), T("stash@{0}"), T("main"), Any, T("Created table")],
                            &[T("stash2"), T("stash@{0}"), T("main"), Any, T("Created table")],
                            &[T("stash2"), T("stash@{1}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('clear', 'stash2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash1"), T("stash@{0}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Clearing and stashing again",
            intent: "After dolt_stash clear, a new push starts the stash list over.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_stash('push', 'myStash');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('clear', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Popping specific stashes",
            intent: "dolt_stash pop of a named stash restores that stash and keeps the others.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A','-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_stash('push', 'myStash');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_stash('push', 'myStash');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_stash('push', 'myStash');",
                "INSERT INTO test VALUES (4, 'd');",
                "SELECT dolt_stash('push', 'myStash');",
                "SELECT dolt_stash('pop', 'myStash', 'stash@{3}');",
                "SELECT dolt_stash('pop', 'myStash', 'stash@{1}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("3"), T("c")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Stashing on different branches",
            intent: "Stashes pushed on different branches all list with their branch.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_stash('push', 'myStash');",
                "SELECT * FROM dolt_checkout('-b', 'br1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_stash('push', 'myStash');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("br1"), Any, T("Created table")],
                            &[T("myStash"), T("stash@{1}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Popping stash onto different branch",
            intent: "A stash pushed on one branch pops onto another branch.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "SELECT dolt_branch('br1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-A', '-m', 'Added a row');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_stash('push', 'myStash');",
                "SELECT * FROM dolt_checkout('br1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("Added a row")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM TEST;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("2"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can drop specific stash",
            intent: "dolt_stash drop removes only the named stash.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_stash('push', 'myStash');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-a', '-m', 'Added 2 b');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_stash('push', 'myStash');",
                "INSERT INTO test VALUES (4, 'd');",
                "SELECT dolt_commit('-a','-m', 'Added 4 d');",
                "INSERT INTO test VALUES (5, 'c');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("Added 2 b")],
                            &[T("myStash"), T("stash@{1}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('drop', 'myStash', 'stash@{1}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("Added 4 d")],
                            &[T("myStash"), T("stash@{1}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can pop into dirty working set without conflict",
            intent: "dolt_stash pop merges into a dirty working set when the changes don't conflict.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_stash('push', 'myStash');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_stash('pop', 'myStash');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can't pop into dirty working set with conflict",
            intent: "dolt_stash pop refuses a dirty working set whose changes conflict, keeping the stash.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A','-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_stash('push', 'myStash');",
                "INSERT INTO test VALUES (1, 'b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Your local changes to the following tables would be overwritten by applying stash 0:\n	{'test'}\nPlease commit your changes or stash them before you merge.\nAborting\nThe stash entry is kept in case you need it again.\n", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can stash modified staged and working set of changes",
            intent: "dolt_stash push saves staged and unstaged changes, and pop restores both.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (2, 'b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("modified")],
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Can use --include-untracked on push",
            intent: "dolt_stash push --include-untracked also saves new tables.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "CREATE TABLE new(id int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("new table")],
                            &[T("public.new"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash', '--include-untracked');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("new table")],
                            &[T("public.new"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Stash with tracked and untracked tables",
            intent: "dolt_stash push without --include-untracked leaves new tables in the working set.",
            set_up_script: &[
                "CREATE TABLE new(i int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "INSERT INTO new VALUES (1),(2);",
                "CREATE TABLE test(id int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.new"), T("t"), T("new table")],
                            &[T("public.test"), T("f"), T("new table")],
                            &[T("public.new"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.new"), T("t"), T("new table")],
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stashing working set with deleted table and popping it",
            intent: "A stash of a dropped table drops it again when popped.",
            set_up_script: &[
                "CREATE TABLE new_tab(id int PRIMARY KEY);",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "DROP TABLE new_tab;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.new_tab"), T("f"), T("deleted")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("new_tab")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
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
            name: "popping stash with deleted table that is deleted already on current head",
            intent: "Popping a stash that drops a table already dropped on HEAD succeeds.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b', 'branch2');",
                "DROP TABLE test;",
                "SELECT dolt_commit('-A','-m','Dropped test');",
                "SELECT * FROM dolt_checkout('branch1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "popping stash with deleted table that the same table exists on current head",
            intent: "Popping a stash that drops a table HEAD still has drops it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "DROP TABLE test;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("deleted")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "popping stash with deleted table that different table with same name on current head gives conflict",
            intent: "Popping a stash that drops a table HEAD has as a different table of the same name conflicts.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT dolt_branch('branch2');",
                "SELECT * FROM dolt_checkout('branch1');",
                "DROP TABLE test;",
                "SELECT dolt_stash('push', 'myStash');",
                "SELECT * FROM dolt_checkout('branch2');",
                "DROP TABLE test;",
                "CREATE TABLE test (id bigint PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge aborted: schema conflict found for table test \n please resolve schema conflicts before merging: \n	table was modified in one branch and deleted in the other", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "popping stash with added table with PK on current head with the exact same table is added already",
            intent: "Popping a stash that adds a table HEAD already has identically succeeds.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_commit('-A', '-m', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('-b',  'branch2');",
                "CREATE TABLE new_test(id int PRIMARY KEY);",
                "INSERT INTO new_test VALUES (1);",
                "SELECT dolt_commit('-A', '-m', 'Created new_test');",
                "SELECT * FROM dolt_checkout('branch1');",
                "CREATE TABLE new_test(id int PRIMARY KEY);",
                "INSERT INTO new_test VALUES (1);",
                "SELECT dolt_add('.');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "simple stash apply",
            intent: "dolt_stash apply restores a stash's changes.",
            set_up_script: &[
                "CREATE TABLE test(id int);",
                "SELECT dolt_stash('push', 'myStash', '-a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('apply', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STATUS;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash apply maintains stash entry",
            intent: "dolt_stash apply keeps the stash in the list.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'First commit');",
                "CREATE TABLE test(id int);",
                "SELECT dolt_stash('push', 'myStash', '-a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("First commit")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('apply', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_STASHES;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("hash", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("myStash"), T("stash@{0}"), T("main"), Any, T("First commit")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "can apply specific stashes",
            intent: "dolt_stash apply of a named stash restores that stash's changes.",
            set_up_script: &[
                "CREATE TABLE test1 (id int);",
                "SELECT dolt_stash('push', 'myStash', '-a');",
                "CREATE TABLE test2 (id int);",
                "SELECT dolt_stash('push', 'myStash', '-a');",
                "CREATE TABLE test3 (id int);",
                "SELECT dolt_stash('push', 'myStash', '-a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('apply', 'myStash', '1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('apply', 'myStash', 'stash@{2}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
                            &[T("public.test1"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_stash_pop_onto_a_view_changed_since_refuses_as_dolt_does() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_stash pop of a view's change onto a commit that changed it too refuses and keeps the stash",
            intent: "Dolt refuses such a pop as local changes its stash would overwrite, naming dolt_schemas where its views live; Doltgrust names the view.",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b INT);",
                "CREATE VIEW v AS SELECT a FROM t;",
                "SELECT dolt_commit('-Am', 'one');",
                "DROP VIEW v;",
                "CREATE VIEW v AS SELECT a, b FROM t;",
                "SELECT dolt_stash('push', 's1');",
                "DROP VIEW v;",
                "CREATE VIEW v AS SELECT b, a FROM t;",
                "SELECT dolt_commit('-am', 'two');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 's1');",
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: "error: Your local changes to the following tables would be overwritten by applying stash 0:\n\t{'view public.v'}\nPlease commit your changes or stash them before you merge.\nAborting\nThe stash entry is kept in case you need it again.\n",
                        ..E
                    }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT name, stash_id FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT)],
                        rows: &[&[T("s1"), T("stash@{0}")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
