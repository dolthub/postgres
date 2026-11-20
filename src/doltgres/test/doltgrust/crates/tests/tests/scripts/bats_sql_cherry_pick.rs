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

//! Dolt's integration-tests/bats/sql-cherry-pick.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_cherry_pick() {
    run_scripts(&[
        ScriptTest {
            name: "sql-cherry-pick: simple cherry pick with the latest commit",
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
            name: "sql-cherry-pick: multiple simple cherry-picks",
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
            name: "sql-cherry-pick: too far back",
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
            name: "sql-cherry-pick: empty commit handling",
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
            name: "sql-cherry-pick: invalid hash",
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
            name: "sql-cherry-pick: has changes in the working set",
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
            name: "sql-cherry-pick: staged changes",
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
            name: "sql-cherry-pick: insert, update, delete rows and schema changes on non existent table in working set",
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
            name: "sql-cherry-pick: error when using --abort with no in-progress cherry-pick",
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
            name: "sql-cherry-pick: commit with CREATE TABLE",
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
            name: "sql-cherry-pick: commit with DROP TABLE",
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
            name: "sql-cherry-pick: commit with ALTER TABLE rename table name",
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
            name: "sql-cherry-pick: cherry-pick commit is a merge commit",
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
            name: "sql-cherry-pick: cherry-pick commit is a cherry-picked commit",
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
            name: "sql-cherry-pick: add triggers",
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
            name: "sql-cherry-pick: add procedures",
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
            name: "sql-cherry-pick: keyless table",
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
            name: "sql-cherry-pick: commit with ALTER TABLE add column",
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
            name: "sql-cherry-pick: commit with ALTER TABLE change column",
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
            name: "sql-cherry-pick: commit with ALTER TABLE modify column",
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
            name: "sql-cherry-pick: commit with ALTER TABLE drop column",
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
            name: "sql-cherry-pick: commit with ALTER TABLE rename column",
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
            name: "sql-cherry-pick: commit with ALTER TABLE drop and add primary key",
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
            name: r#"sql-cherry-pick: conflict resolution"#,
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
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_other and dolt_conflicts_test")),
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
                    dolt: Some(Dolt::MySqlOnly("dolt conflicts cat is a CLI command, so the port reads dolt_conflicts_other and dolt_conflicts_test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_cherry_pick('--abort');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1;"#,
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
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status ORDER BY 1;"#,
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
                    query: r#"SELECT * FROM test;"#,
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
    ]);
}
