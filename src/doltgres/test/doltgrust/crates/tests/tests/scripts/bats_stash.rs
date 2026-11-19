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

//! Dolt's integration-tests/bats/stash.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_stash() {
    run_scripts(&[
        ScriptTest {
            name: "stash: stashing on clean working set",
            intent: "Stashing with no changes fails and stashes nothing.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "No local changes to save", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: simple stashing and popping stash",
            intent: "A stash takes the working changes away and popping it brings them back.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("main"), T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: clearing stash when stash list is empty",
            intent: "Clearing an empty stash list leaves it empty.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('clear', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: clearing stash removes all entries in stash list",
            intent: "Clearing the stash list removes every stash.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
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
                    query: "INSERT INTO test VALUES (3, 'c');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('clear', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: clearing stash and stashing again",
            intent: "Stashing after clearing starts a new list.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
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
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: clearing stash and popping returns error of no entries found",
            intent: "Popping after clearing fails, and stashing again works.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
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
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('clear', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "No stash entries found.", ..E }),
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
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: popping oldest stash",
            intent: "Popping the oldest stash by index applies its changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Added row 2 b');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 'c');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("main"), T("Added row 2 b")],
                            &[T("stash@{1}"), T("main"), T("Created table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash', 'stash@{1}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
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
            name: "stash: popping neither latest nor oldest stash",
            intent: "Popping a middle stash by index applies it, from any branch.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Added row 2 b');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 'c');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 'd');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Added row 4 d');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (5, 'e');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash', 'stash@{1}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("4"), T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash', 'stash@{1}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                            &[T("4"), T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: stashing multiple entries on different branches",
            intent: "Stashes from different branches share one list and name their branches.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("newbranch"), T("Created table")],
                            &[T("stash@{1}"), T("main"), T("Created table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: popping stash on different branch",
            intent: "A stash made on one branch pops onto another.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("main"), T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('newbranch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: dropping stash removes an entry at given index in stash list",
            intent: "Dropping a stash by index removes only that stash.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Added row 2 b');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 'c');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 'd');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Added row 4 d');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (5, 'e');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("main"), T("Added row 4 d")],
                            &[T("stash@{1}"), T("main"), T("Added row 2 b")],
                            &[T("stash@{2}"), T("main"), T("Created table")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('drop', 'myStash', 'stash@{1}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("main"), T("Added row 4 d")],
                            &[T("stash@{1}"), T("main"), T("Created table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: popping stash on dirty working set with no conflict",
            intent: "A stash pops onto other working changes that don't conflict.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("2"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash', 'stash@{0}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
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
            name: "stash: popping stash on dirty working set with conflict",
            intent: "A stash that conflicts with working changes doesn't pop and is kept.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Your local changes to the following tables would be overwritten by applying stash 0:\n	{'test'}\nPlease commit your changes or stash them before you merge.\nAborting\nThe stash entry is kept in case you need it again.\n", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: stashing both modified staged and working set of changes and popping the stash",
            intent: "Staged and working changes both stash, and pop back as working changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
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
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
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
            name: "stash: stashing on working set with untracked tables only should be nothing to stash",
            intent: "Untracked tables alone give nothing to stash.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE new_table (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "No local changes to save", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("new_table")],
                            &[T("test")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: stashing untracked tables with --include-untracked flag and popping the table should not be staged",
            intent: "--include-untracked stashes untracked tables, which pop back untracked.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE new_table (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test_table (id int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash', '--include-untracked');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: stashing staged new table changes and popping the stash, the added table should be staged",
            intent: "A staged new table stashes and pops back staged.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE new_table (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("new_table")],
                            &[T("test")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("new_table")],
                            &[T("test")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: stashing with staged, working set changes with untracked tables should only stash modified working set and staged changes",
            intent: "A stash takes staged and working changes and leaves untracked tables.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE new_table (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO new_table VALUES (1),(2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test_table (id int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                            &[T("test_table")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("new_table")],
                            &[T("test")],
                            &[T("test_table")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: stashing working set with deleted table and popping it",
            intent: "A dropped table stashes and pops back dropped.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE new_table (id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'create new table');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE new_table;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("new_table")],
                            &[T("test")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: simple stashing and popping stash after running GC",
            intent: "A stash survives garbage collection.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add row of 2b');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("2"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("main"), T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
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
            name: "stash: popping stash with deleted table that is deleted already on current head",
            intent: "Popping a drop of a table the branch already dropped changes nothing.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'table ''test'' is dropped');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
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
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("br1"), T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: popping stash with deleted table that the same table exists on current head",
            intent: "Popping a drop of a table the branch has drops it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
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
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("br1"), T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: popping stash with deleted table that different table with same name on current head gives conflict",
            intent: "Popping a drop of a table the branch replaced fails as a schema conflict.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
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
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (id bigint PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("br1"), T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge aborted: schema conflict found for table test \n please resolve schema conflicts before merging: \n	table was modified in one branch and deleted in the other", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: popping stash with added table with PK on current head with the exact same table is added already",
            intent: "Popping a new table the branch already has unchanged changes nothing.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE new_test(id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO new_test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'new table ''new_test'' is created');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE new_test(id int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO new_test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("br1"), T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: popping stash with added keyless table on current head with the exact same table is added already",
            intent: "Popping a new keyless table the branch already has unchanged changes nothing.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE new_test(id int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO new_test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'new table ''new_test'' is created');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE new_test(id int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO new_test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('br2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stash_id, branch, commit_message FROM dolt_stashes ORDER BY stash_id;",
                    expected: Expected::Rows {
                        columns: &[Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[
                            &[T("stash@{0}"), T("br1"), T("Created table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt stash list is the CLI's, so dolt_stashes lists the stashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 'myStash');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Bug("Dolt main refuses this pop as local changes to new_test, where the stash holds the same table as the branch, so it applies cleanly; Dolt skips this part (\"stash of the exact copy of keyless table causes merge conflict, where it should not\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt status is the CLI's, so dolt_status lists the changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "stash: can apply stashes at specific indices",
            intent: "Stashes apply by index.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test1 (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash', '-a');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("dolt stash is the CLI's, and dolt_stash names its stash, myStash here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash', '-a');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test3 (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('push', 'myStash', '-a');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('apply', 'myStash', 'stash@{1}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('apply', 'myStash', 'stash@{2}');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                            &[T("test1")],
                            &[T("test2")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is the CLI's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
