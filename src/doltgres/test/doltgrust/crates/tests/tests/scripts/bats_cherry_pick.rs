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

//! Dolt's integration-tests/bats/cherry-pick.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_cherry_pick() {
    run_scripts(&[
        ScriptTest {
            name: "cherry-pick: simple cherry pick with the latest commit",
            intent: "dolt_cherry_pick of a branch applies only its head commit's changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("3"), T("c")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: multiple simple cherry-picks",
            intent: "Cherry-picking two commits in turn applies each one's changes.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v = 'x' WHERE pk = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (5, 'g'), (8, 'u');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Updated 2b to 2x and inserted more rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1~2');",
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
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("2"), T("x")],
                            &[T("5"), T("g")],
                            &[T("8"), T("u")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: too far back",
            intent: "Cherry-picking an ancestor past the first commit fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1~10');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: empty commit handling",
            intent: "Cherry-picking an empty commit fails unless --allow-empty is given.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-am', 'empty commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "The previous cherry-pick commit is empty. Use --allow-empty to cherry-pick empty commits.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--allow-empty', 'branch1');",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: invalid hash",
            intent: "Cherry-picking a hash naming no commit fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: has changes in the working set",
            intent: "Cherry-picking over uncommitted changes fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 'f');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1~2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot cherry-pick with uncommitted changes", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: staged changes",
            intent: "Cherry-picking over staged changes fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 'f');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1~2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot cherry-pick with uncommitted changes", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: insert, update, delete rows and schema changes on non existent table in working set",
            intent: "Cherry-picking changes to a table the current branch doesn't have fails as a schema conflict and leaves the tables as they were.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE branch1table (id int PRIMARY KEY, col1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO branch1table VALUES (9,8),(7,6),(5,4);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'create table with rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO branch1table VALUES (1,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Insert a row');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge aborted: schema conflict found for table branch1table \n please resolve schema conflicts before merging: \n	table was modified in one branch and deleted in the other", ..E }),
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
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE branch1table SET col1 = 0 WHERE id > 6;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Update a rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge aborted: schema conflict found for table branch1table \n please resolve schema conflicts before merging: \n	table was modified in one branch and deleted in the other", ..E }),
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
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM branch1table WHERE id > 8;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Update and delete rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge aborted: schema conflict found for table branch1table \n please resolve schema conflicts before merging: \n	table was modified in one branch and deleted in the other", ..E }),
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
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE branch1table ADD COLUMN col2 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Alter table add column');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge aborted: schema conflict found for table branch1table \n please resolve schema conflicts before merging: \n	table was modified in one branch and deleted in the other", ..E }),
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
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: error when using --abort with no in-progress cherry-pick",
            intent: "--abort with no cherry-pick in progress fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no cherry-pick merge to abort", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: commit with CREATE TABLE",
            intent: "Cherry-picking a commit that creates a table brings the table and its rows.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE table_a (pk bigint PRIMARY KEY, v varchar(10));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO table_a VALUES (11, 'aa'), (22, 'ab'), (33, 'ac');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Added table_a with rows and delete pk=2 from test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("table_a")],
                            &[T("test")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
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
                    query: "SELECT * FROM table_a ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("11"), T("aa")],
                            &[T("22"), T("ab")],
                            &[T("33"), T("ac")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: commit with DROP TABLE",
            intent: "Cherry-picking a commit that drops a table the current branch changed fails and keeps the table.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Drop table test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("generated_foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
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
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge aborted: schema conflict found for table test \n please resolve schema conflicts before merging: \n	table was modified in one branch and deleted in the other", ..E }),
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
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: commit with ALTER TABLE rename table name",
            intent: "Cherry-picking a table rename renames the table.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME TO new_name;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'rename table test to new_name');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("new_name")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's, so information_schema.tables lists the tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: cherry-pick commit is a merge commit",
            intent: "Cherry-picking a merge commit fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('branch2', 'branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 'd'), (5, 'e');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add more rows in branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (6, 'f'), (7, 'g');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add more rows in branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('branch2', '-m', 'merge branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cherry-picking a merge commit is not supported", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: cherry-pick commit is a cherry-picked commit",
            intent: "A commit made by a cherry-pick can be cherry-picked again.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('branch2', 'branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 'd'), (5, 'e');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add more rows in branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (6, 'f'), (7, 'g');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add more rows in branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch2');",
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
                            &[T("3"), T("c")],
                            &[T("4"), T("d")],
                            &[T("5"), T("e")],
                            &[T("6"), T("f")],
                            &[T("7"), T("g")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("4"), T("d")],
                            &[T("5"), T("e")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: add triggers",
            intent: "Cherry-picking commits that add and drop a trigger adds and drops it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION trigger1_fn() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.v := NEW.v || ' inserted'; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's trigger body is a PL/pgSQL trigger function here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER trigger1 BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger1_fn();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW two AS SELECT 1+1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4,'z');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("4"), T("z inserted")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add trigger');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname FROM pg_trigger WHERE tgrelid = 'test'::regclass AND NOT tgisinternal;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TRIGGERS is MySQL's, so pg_trigger lists the table's triggers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("4"), T("z inserted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname FROM pg_trigger WHERE tgrelid = 'test'::regclass AND NOT tgisinternal;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                            &[T("trigger1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TRIGGERS is MySQL's, so pg_trigger lists the table's triggers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER trigger1 ON test;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'drop trigger');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname FROM pg_trigger WHERE tgrelid = 'test'::regclass AND NOT tgisinternal;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TRIGGERS is MySQL's, so pg_trigger lists the table's triggers")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: drop triggers",
            intent: "Cherry-picking a commit that drops a trigger drops it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION trigger1_fn() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.v := NEW.v || ' inserted'; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's trigger body is a PL/pgSQL trigger function here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER trigger1 BEFORE INSERT ON test FOR EACH ROW EXECUTE FUNCTION trigger1_fn();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4,'z');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("4"), T("z inserted")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add trigger');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname FROM pg_trigger WHERE tgrelid = 'test'::regclass AND NOT tgisinternal;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TRIGGERS is MySQL's, so pg_trigger lists the table's triggers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("4"), T("z inserted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname FROM pg_trigger WHERE tgrelid = 'test'::regclass AND NOT tgisinternal;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                            &[T("trigger1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TRIGGERS is MySQL's, so pg_trigger lists the table's triggers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER trigger1 ON test;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'drop trigger');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgname FROM pg_trigger WHERE tgrelid = 'test'::regclass AND NOT tgisinternal;",
                    expected: Expected::Rows {
                        columns: &[Column("tgname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW TRIGGERS is MySQL's, so pg_trigger lists the table's triggers")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: add procedures",
            intent: "Cherry-picking a commit that adds a function adds it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION proc1(x int) RETURNS int LANGUAGE sql AS 'SELECT x';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's stored procedure that selects its argument is a SQL function here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proc1(434);",
                    expected: Expected::Rows {
                        columns: &[Column("proc1", INT4)],
                        rows: &[
                            &[T("434")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add procedure');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname FROM pg_proc WHERE proname = 'proc1';",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW PROCEDURE STATUS is MySQL's, so pg_proc lists the function")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname FROM pg_proc WHERE proname = 'proc1';",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME)],
                        rows: &[
                            &[T("proc1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW PROCEDURE STATUS is MySQL's, so pg_proc lists the function")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proc1(434);",
                    expected: Expected::Rows {
                        columns: &[Column("proc1", INT4)],
                        rows: &[
                            &[T("434")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: keyless table",
            intent: "Cherry-picking a commit leaves a table it doesn't touch unchanged.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE keyless (id int, name varchar(10));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add keyless table');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (1,'1'), (2,'3');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'insert into keyless table');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
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
            name: "cherry-pick: commit with ALTER TABLE add column",
            intent: "Cherry-picking an added column adds it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD COLUMN c int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'alter table test add column c');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                            &[T("v"), T("character varying")],
                            &[T("c"), T("integer")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so information_schema.columns lists the columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: commit with ALTER TABLE change column",
            intent: "Cherry-picking a renamed and widened column renames it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN v TO c;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CHANGE COLUMN renames and retypes a column in one statement, which is RENAME COLUMN and ALTER COLUMN ... TYPE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c TYPE varchar(100);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'alter table test change column v');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                            &[T("c"), T("character varying")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so information_schema.columns lists the columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: commit with ALTER TABLE modify column",
            intent: "Cherry-picking an incompatible column type change fails as a schema conflict.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v = '1' WHERE pk < 4;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v TYPE int USING v::int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's MODIFY COLUMN is ALTER COLUMN ... TYPE, converting with USING")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'alter table test modify column v');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merge aborted: schema conflict found for table test", message_contains: true, ..E }),
                    dolt: Some(Dolt::Differs("Dolt's message names its MySQL column types, so the port matches the message's start")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: commit with ALTER TABLE drop column",
            intent: "Cherry-picking a dropped column drops it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN v;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'alter table test drop column v');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so information_schema.columns lists the columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: commit with ALTER TABLE rename column",
            intent: "Cherry-picking a renamed column renames it.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN v TO c;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'alter table test rename column v');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                            &[T("c"), T("character varying")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so information_schema.columns lists the columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: commit with ALTER TABLE drop and add primary key",
            intent: "Cherry-picking a primary key change fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT test_pkey, ADD PRIMARY KEY (pk, v);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's DROP PRIMARY KEY is DROP CONSTRAINT of the key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'alter table test drop and add primary key');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[&[Any, T("0"), T("0"), T("0")]],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the cherry-pick with \"cannot merge because table test has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: author and timestamp preserved during cherry-pick",
            intent: "A cherry-picked commit keeps the original author and author date.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (99, 'auth');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--author', 'Original Author <original@example.com>', '--date', '2023-09-26T01:23:45', '-m', 'commit with specific author');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.commit', dolt_hashof('HEAD'), false);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit'));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("commit with specific author"), T("Original Author"), T("original@example.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT author, author_email, author_date FROM dolt_log WHERE message = 'commit with specific author';",
                    expected: Expected::Rows {
                        columns: &[Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMPTZ)],
                        rows: &[
                            &[T("Original Author"), T("original@example.com"), T("2023-09-26 01:23:45+00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: multiple authors preserved in sequence with merge workflow",
            intent: "Cherry-picked commits keep their authors through a later merge.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('branch1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (200, 'alice');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--author', 'Alice Developer <alice@company.com>', '-m', 'Alice''s feature');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.alice', dolt_hashof('HEAD'), false);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (201, 'bob');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--author', 'Bob Engineer <bob@company.com>', '-m', 'Bob''s improvement');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.bob', dolt_hashof('HEAD'), false);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (202, 'carol');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--author', 'Carol Architect <carol@company.com>', '-m', 'Carol''s refactor');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.carol', dolt_hashof('HEAD'), false);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.alice'));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Alice's feature"), T("Alice Developer"), T("alice@company.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.bob'));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Bob's improvement"), T("Bob Engineer"), T("bob@company.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.carol'));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Carol's refactor"), T("Carol Architect"), T("carol@company.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('integration_branch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('integration_branch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (300, 'merge');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--author', 'Integration Manager <integration@company.com>', '-m', 'prepare for merge');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('integration_branch', '--no-ff', '-m', 'Merge integration_branch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message, author, author_email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("author", TEXT), Column("author_email", TEXT)],
                        rows: &[
                            &[T("Merge integration_branch"), T("postgres"), T("postgres@localhost")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("a merge commit's author is the session's user, postgres here, where Dolt's test server runs as its configured user")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT author, author_email, message FROM dolt_log WHERE author IN ('Alice Developer', 'Bob Engineer', 'Carol Architect', 'Integration Manager') ORDER BY author;",
                    expected: Expected::Rows {
                        columns: &[Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("Alice Developer"), T("alice@company.com"), T("Alice's feature")],
                            &[T("Bob Engineer"), T("bob@company.com"), T("Bob's improvement")],
                            &[T("Carol Architect"), T("carol@company.com"), T("Carol's refactor")],
                            &[T("Integration Manager"), T("integration@company.com"), T("prepare for merge")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: --continue with no active cherry-pick",
            intent: "--continue with no cherry-pick in progress fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));",
                "CREATE INDEX ON test (v);",
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Created table');",
                "SELECT dolt_branch('branch1');",
                "SELECT * FROM dolt_checkout('branch1');",
                "INSERT INTO test VALUES (1, 'a');",
                "SELECT dolt_commit('-am', 'Inserted 1');",
                "INSERT INTO test VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'Inserted 2');",
                "INSERT INTO test VALUES (3, 'c');",
                "SELECT dolt_commit('-am', 'Inserted 3');",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no cherry-pick merge to continue", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cherry-pick: schema change, with data conflict"#,
            intent: r#"A cherry-pick that drops a column and updates a row our branch lacks applies its other changes and leaves that row as a conflict, which resolving with ours clears."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));"#,
                r#"CREATE INDEX ON test (v);"#,
                r#"INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'Created table');"#,
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT * FROM dolt_checkout('branch1');"#,
                r#"INSERT INTO test VALUES (1, 'a');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 'b');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 'c');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"CREATE TABLE generated_foo (pk int PRIMARY KEY);"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"CREATE TABLE other (pk int PRIMARY KEY, c1 int, c2 int);"#,
                r#"INSERT INTO other VALUES (1, 2, 3);"#,
                r#"SELECT dolt_commit('-Am', 'add other table (on main)');"#,
                r#"SELECT dolt_branch('branch2');"#,
                r#"SELECT * FROM dolt_checkout('branch2');"#,
                r#"INSERT INTO other VALUES (100, 200, 300);"#,
                r#"SELECT dolt_commit('-am', 'add row 100 to other (on branch2)');"#,
                r#"ALTER TABLE other DROP COLUMN c1;"#,
                r#"UPDATE other SET c2 = 400 WHERE pk = 100;"#,
                r#"INSERT INTO other VALUES (10, 30);"#,
                r#"INSERT INTO test VALUES (100, 'q');"#,
                r#"SELECT dolt_commit('-am', 'alter table, add row 10 to other, add row 100 to test (on branch2)');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('branch2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt cherry-pick is a CLI command, whose \"Unable to apply commit cleanly\" error is dolt_cherry_pick's counts, and the CLI keeps a cherry-pick's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.other"), T("f"), T("conflict")],
                            &[T("public.other"), T("f"), T("modified")],
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT base_pk, base_c1, base_c2, our_pk, our_c2, dolt_our_diff_type, their_pk, their_c2, dolt_their_diff_type FROM dolt_conflicts_other;"#,
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", INT4), Column("base_c2", INT4), Column("our_pk", INT4), Column("our_c2", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c2", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("100"), T("200"), T("300"), Null, Null, T("removed"), T("100"), T("400"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads the table's dolt_conflicts_ view")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM other ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("10"), T("30")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', 'other');"#,
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
                    query: r#"SELECT count(*) FROM dolt_conflicts;"#,
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
                    query: r#"SELECT dolt_commit('-m', 'cherry-picked HEAD commit from branch2');"#,
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cherry-pick: foreign key violation"#,
            intent: r#"A cherry-pick whose row breaks a foreign key applies with the violation recorded, and an abort undoes it all."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));"#,
                r#"CREATE INDEX ON test (v);"#,
                r#"INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'Created table');"#,
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT * FROM dolt_checkout('branch1');"#,
                r#"INSERT INTO test VALUES (1, 'a');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 'b');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 'c');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"CREATE TABLE generated_foo (pk int PRIMARY KEY);"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SELECT * FROM dolt_checkout('branch1');"#,
                r#"CREATE UNIQUE INDEX test_v_key ON test (v);"#,
                r#"CREATE TABLE other (pk int PRIMARY KEY, v varchar(10), FOREIGN KEY (v) REFERENCES test(v));"#,
                r#"INSERT INTO other VALUES (1, 'a');"#,
                r#"SELECT dolt_commit('-Am', 'add other table (on branch1)');"#,
                r#"SELECT dolt_branch('branch2', 'branch1');"#,
                r#"SELECT * FROM dolt_checkout('branch2');"#,
                r#"SET session_replication_role = replica;"#,
                r#"UPDATE other SET v = 'z' WHERE pk = 1;"#,
                r#"RESET session_replication_role;"#,
                r#"INSERT INTO test VALUES (100, 'q');"#,
                r#"SELECT dolt_commit('-Am', 'update row 1 in other and insert row 100 into test (on branch2)');"#,
                r#"SELECT * FROM dolt_checkout('branch1');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('branch2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt cherry-pick is a CLI command, whose \"Unable to apply commit cleanly\" error is dolt_cherry_pick's counts, and the CLI keeps a cherry-pick's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.other"), T("f"), T("constraint violation")],
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.other"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_violation_type, pk, v FROM dolt_constraint_violations_other;"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("z")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--abort');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_constraint_violations;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM other;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cherry-pick: conflict resolution"#,
            intent: r#"A cherry-pick whose rows conflict in two tables stops with their conflicts, an abort undoes it, and resolving the conflicts with theirs lets the result commit."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));"#,
                r#"CREATE INDEX ON test (v);"#,
                r#"INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'Created table');"#,
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT * FROM dolt_checkout('branch1');"#,
                r#"INSERT INTO test VALUES (1, 'a');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 'b');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 'c');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"CREATE TABLE generated_foo (pk int PRIMARY KEY);"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SELECT * FROM dolt_checkout('branch1');"#,
                r#"CREATE TABLE other (pk int PRIMARY KEY, v int);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO other VALUES (1, 2);"#,
                r#"INSERT INTO test VALUES (4,'f');"#,
                r#"SELECT dolt_commit('-am', 'add other table');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"CREATE TABLE other (pk int PRIMARY KEY, v int);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO other VALUES (1, 3);"#,
                r#"INSERT INTO test VALUES (4,'k');"#,
                r#"SELECT dolt_commit('-am', 'add other table with conflict and test with conflict');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("2"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt cherry-pick is a CLI command, whose \"Unable to apply commit cleanly\" error is dolt_cherry_pick's counts, and the CLI keeps a cherry-pick's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_v, their_pk, their_v FROM dolt_conflicts_other;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT4), Column("our_v", INT4), Column("their_pk", INT4), Column("their_v", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads the table's dolt_conflicts_ view")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT our_pk, our_v, their_pk, their_v FROM dolt_conflicts_test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT8), Column("our_v", VARCHAR), Column("their_pk", INT8), Column("their_v", VARCHAR)],
                        rows: &[
                            &[T("4"), T("k"), T("4"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads the table's dolt_conflicts_ view")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--abort');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("2"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt cherry-pick is a CLI command, whose \"Unable to apply commit cleanly\" error is dolt_cherry_pick's counts, and the CLI keeps a cherry-pick's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1, 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.other"), T("f"), T("conflict")],
                            &[T("public.test"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', '.');"#,
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
                    query: r#"SELECT * FROM other;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE pk = 4;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("4"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'committing cherry-picked change');"#,
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cherry-pick: --continue after resolving conflicts"#,
            intent: r#"After its conflicts are resolved and staged, --continue commits the cherry-pick with the original commit's author and message, and --continue with --abort is refused."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));"#,
                r#"CREATE INDEX ON test (v);"#,
                r#"INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'Created table');"#,
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT * FROM dolt_checkout('branch1');"#,
                r#"INSERT INTO test VALUES (1, 'a');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 'b');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 'c');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"CREATE TABLE generated_foo (pk int PRIMARY KEY);"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SELECT dolt_branch('continue_test');"#,
                r#"SELECT * FROM dolt_checkout('continue_test');"#,
                r#"INSERT INTO test VALUES (100, 'branch1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('--author', 'Feature Dev <feature@example.com>', '--date', '2022-01-01T12:00:00', '-m', 'Add row from branch1');"#,
                r#"SELECT set_config('test.commit1', dolt_hashof('HEAD'), false);"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"INSERT INTO test VALUES (100, 'main');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'Add row from main');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick(current_setting('test.commit1'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt cherry-pick is a CLI command, whose \"Unable to apply commit cleanly\" error is dolt_cherry_pick's counts, and the CLI keeps a cherry-pick's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET v = 'resolved' WHERE pk = 100;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_conflicts_test;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMIT;"#,
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--continue', '--abort');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"error: --continue and --abort are mutually exclusive"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--continue');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT author, author_email, message FROM dolt_log LIMIT 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("author", TEXT), Column("author_email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("Feature Dev"), T("feature@example.com"), T("Add row from branch1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt log -n 1 is a CLI command, so the port reads dolt_log")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE pk = 100;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("100"), T("resolved")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cherry-pick: --continue with unresolved conflicts"#,
            intent: r#"--continue refuses while the cherry-pick's conflicts are unresolved."#,
            set_up_script: &[
                r#"CREATE TABLE test(pk bigint PRIMARY KEY, v varchar(10));"#,
                r#"CREATE INDEX ON test (v);"#,
                r#"INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'Created table');"#,
                r#"SELECT dolt_branch('branch1');"#,
                r#"SELECT * FROM dolt_checkout('branch1');"#,
                r#"INSERT INTO test VALUES (1, 'a');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 1');"#,
                r#"INSERT INTO test VALUES (2, 'b');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 2');"#,
                r#"INSERT INTO test VALUES (3, 'c');"#,
                r#"SELECT dolt_commit('-am', 'Inserted 3');"#,
                r#"CREATE TABLE generated_foo (pk int PRIMARY KEY);"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"SELECT dolt_branch('continue_test2');"#,
                r#"SELECT * FROM dolt_checkout('continue_test2');"#,
                r#"INSERT INTO test VALUES (100, 'branch1');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'Add row from branch1');"#,
                r#"SELECT set_config('test.commit1', dolt_hashof('HEAD'), false);"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"INSERT INTO test VALUES (100, 'main');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'Add row from main');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SET dolt_force_transaction_commit = 1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick(current_setting('test.commit1'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt cherry-pick is a CLI command, whose \"Unable to apply commit cleanly\" error is dolt_cherry_pick's counts, and the CLI keeps a cherry-pick's conflicts as dolt_allow_commit_conflicts and dolt_force_transaction_commit do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--continue');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt cherry-pick --continue is a CLI command, which fails on the counts dolt_cherry_pick('--continue') returns while conflicts remain")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
