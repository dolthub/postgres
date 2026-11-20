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

//! Dolt's integration-tests/bats/sql-server.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_server() {
    run_scripts(&[
        ScriptTest {
            name: r#"sql-server: test reset_hard"#,
            intent: r#"dolt_reset('--hard') drops the working set's changes, in its own statement or after an insert in the same request."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY, c0 INT);"#,
                r#"INSERT INTO test VALUES (1, 1), (2, 2), (3, 3);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'added table test');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (7, 7);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T(r#"public.test"#), T(r#"f"#), T(r#"modified"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sum(pk), sum(c0) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T(r#"6"#), T(r#"6"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (8, 8);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sum(pk), sum(c0) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T(r#"6"#), T(r#"6"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: test multi db with use statements"#,
            intent: r#"Two databases of one server keep their own tables through inserts, deletes, and updates."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE repo2;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r1_one_pk (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r1_one_pk (pk) VALUES (0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r1_one_pk (pk, c1) VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r1_one_pk (pk, c1, c2) VALUES (2, 2, 2), (3, 3, 3);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r2_one_pk (pk BIGINT NOT NULL, c3 BIGINT, c4 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_one_pk (pk) VALUES (0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_one_pk (pk, c3) VALUES (1, 1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_one_pk (pk, c3, c4) VALUES (2, 2, 2), (3, 3, 3);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r1_one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T(r#"0"#), Null, Null],
                            &[T(r#"1"#), T(r#"1"#), Null],
                            &[T(r#"2"#), T(r#"2"#), T(r#"2"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"3"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r2_one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c3", INT8), Column("c4", INT8)],
                        rows: &[
                            &[T(r#"0"#), Null, Null],
                            &[T(r#"1"#), T(r#"1"#), Null],
                            &[T(r#"2"#), T(r#"2"#), T(r#"2"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"3"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM r1_one_pk WHERE pk = 0;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM r2_one_pk WHERE pk = 0;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r1_one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#), Null],
                            &[T(r#"2"#), T(r#"2"#), T(r#"2"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"3"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r2_one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c3", INT8), Column("c4", INT8)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#), Null],
                            &[T(r#"2"#), T(r#"2"#), T(r#"2"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"3"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE r1_one_pk SET c2 = 1 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE r2_one_pk SET c4 = 1 WHERE pk = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r1_one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#), T(r#"1"#)],
                            &[T(r#"2"#), T(r#"2"#), T(r#"2"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"3"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM r2_one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c3", INT8), Column("c4", INT8)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#), T(r#"1"#)],
                            &[T(r#"2"#), T(r#"2"#), T(r#"2"#)],
                            &[T(r#"3"#), T(r#"3"#), T(r#"3"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: DOLT_ADD, DOLT_COMMIT, DOLT_CHECKOUT, DOLT_MERGE work together in server mode"#,
            intent: r#"Sessions on the default branch see only main's rows while another branch's changes stay on it, until a commit and a merge bring them."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"each dolt sql call is its own session on the default branch, so the port gives each a connection of its own"#)),
                    client: "s1",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "s2",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s3",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'Step 1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s4",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'feature-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature-branch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s5",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"0"#)],
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s6",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature-branch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s7",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s7",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (4);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s7",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (21232);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s7",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM test WHERE pk = 4;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s7",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET pk = 21 WHERE pk = 21232;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s7",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"0"#)],
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "s8",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('feature-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature-branch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s9",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'Insert 3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s9",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (500000);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s10",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (500001);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "s10",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM test WHERE pk = 500001;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "s10",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET pk = 60 WHERE pk = 500000;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "s10",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s10",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Insert 60');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s10",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('feature-branch', '-m', 'merge feature-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"0"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "s10",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"0"#)],
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                            &[T(r#"3"#)],
                            &[T(r#"21"#)],
                            &[T(r#"60"#)],
                        ],
                        tag: "SELECT 6",
                    },
                    client: "s11",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "s12",
                    database: "postgres",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: dolt_branch -d won't delete a db's default branch"#,
            intent: r#"The default branch can't be deleted, even from another branch."#,
            set_up_script: &[
                r#"SELECT dolt_branch('branch1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('branch1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'branch1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('-D', 'main');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unable to delete branch 'main', because it is the default branch for database 'postgres'; this can by changed on the command line, by stopping the sql-server, running `dolt checkout <another_branch> and restarting the sql-server"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: DOLT_MERGE ff works"#,
            intent: r#"A merge of a branch ahead of main fast-forwards it."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Step 1');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('-b', 'feature-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'feature-branch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE test SET pk = 1000 WHERE pk = 0;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'this is a ff');"#,
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
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('feature-branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                            &[T(r#"3"#)],
                            &[T(r#"1000"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "after",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT COUNT(*) FROM dolt_log;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "after",
                    database: "postgres",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: connect to another branch with connection string"#,
            intent: r#"A connection to db/branch works on that branch, leaving main alone."#,
            set_up_script: &[
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT, c1 INT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "fb",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"test"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "fb2",
                    database: "postgres/feature-branch",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: connect to a commit with connection string"#,
            intent: r#"A connection to a commit reads it and refuses writes and new branches."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-a', '-m', 'Created new table');"#,
                r#"INSERT INTO test VALUES (1), (2), (3);"#,
                r#"SELECT dolt_commit('-a', '-m', 'Inserted 3 values');"#,
                r#"INSERT INTO test VALUES (4), (5), (6);"#,
                r#"SELECT dolt_commit('-a', '-m', 'Inserted 3 more values');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"the test names the commit by its hash, and the port by main~1, the same commit"#)),
                    client: "c1",
                    database: "postgres/main~1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (7);"#,
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT in a read-only transaction", ..E }),
                    client: "c1",
                    database: "postgres/main~1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'newBranch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'newBranch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs(r#"a session on a commit may run dolt_branch and dolt_checkout (Appendix C, revision connections), where Dolt refuses a new branch in a read-only database"#)),
                    client: "c1",
                    database: "postgres/main~1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c1",
                    database: "postgres/main~1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: auto increment is globally distinct across branches and connections"#,
            intent: r#"An identity column's next value is shared across branches and connections, and starts over when the table is made again."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (pk BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's AUTO_INCREMENT is an identity column"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 (val) VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "m",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 (val) VALUES (2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                            &[T(r#"2"#), T(r#"2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "m",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'table with two values');"#,
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
                    query: r#"SELECT dolt_branch('new_branch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 (val) VALUES (3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "nb",
                    database: "postgres/new_branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                            &[T(r#"2"#), T(r#"2"#)],
                            &[T(r#"3"#), T(r#"3"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "nb",
                    database: "postgres/new_branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 (val) VALUES (4);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                            &[T(r#"2"#), T(r#"2"#)],
                            &[T(r#"4"#), T(r#"4"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "m",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE t1;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (pk BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, val INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 (val) VALUES (4);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1 ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("val", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"4"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "m",
                    database: "postgres",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: can't drop branch qualified database names"#,
            intent: r#"Dropping a database ends its branches' connections, and a branch's name can't be dropped as a database."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test1;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test2;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test3;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a (x INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table a');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a (x INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "test2",
                    database: "test2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test2",
                    database: "test2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (3), (4);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "test2",
                    database: "test2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table a');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test2",
                    database: "test2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a (x INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "test3",
                    database: "test3",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test3",
                    database: "test3",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a VALUES (5), (6);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "test3",
                    database: "test3",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table a');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test3",
                    database: "test3",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('newbranch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM a ORDER BY x;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b1",
                    database: "test1/newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('newbranch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test2",
                    database: "test2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM a ORDER BY x;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T(r#"3"#)],
                            &[T(r#"4"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b2",
                    database: "test2/newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE TEST1 WITH (FORCE);"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    dolt: Some(Dolt::MySqlOnly(r#"Postgres refuses to drop a database other sessions are connected to without FORCE"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM a;"#,
                    expected: Expected::ClientError("does not exist"),
                    client: "b1again",
                    database: "test1/newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE "test2/newbranch";"#,
                    expected: Expected::Error(Diagnostic { code: "3D000", message: r#"database "test2/newbranch" does not exist"#, ..E }),
                    dolt: Some(Dolt::Differs(r#"a branch is a revision of a database, not a database of its own (Appendix C, revision connections), so DROP DATABASE finds none, where Dolt refuses to drop a revision database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE TEST2 WITH (FORCE);"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM a;"#,
                    expected: Expected::ClientError("does not exist"),
                    client: "b2again",
                    database: "test2/newbranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM a ORDER BY x;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T(r#"5"#)],
                            &[T(r#"6"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "test3",
                    database: "test3",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: CALL DOLT_BRANCH -m on session active branch (dolt sql-server)"#,
            intent: r#"Renaming the session's own branch moves the session to the new name."#,
            set_up_script: &[
                r#"SELECT dolt_branch('other');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('other');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'other'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('-m', 'other', 'newOther');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
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
                            &[T(r#"newOther"#)],
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
                            &[T(r#"main"#)],
                            &[T(r#"newOther"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: dirty branches listed properly in dolt_branches table"#,
            intent: r#"dolt_branches marks the branches whose working sets hold uncommitted changes."#,
            set_up_script: &[
                r#"SELECT dolt_branch('br1');"#,
                r#"SELECT dolt_branch('br2');"#,
                r#"SELECT dolt_branch('br3');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE tbl (i INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "br1",
                    database: "postgres/br1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'commit it');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "br1",
                    database: "postgres/br1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE tbl (j INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "br2",
                    database: "postgres/br2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('br1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T(r#"1"#), T(r#"0"#), T(r#"merge successful"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "br3",
                    database: "postgres/br3",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE othertbl (k INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "br3",
                    database: "postgres/br3",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, dirty FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("dirty", BOOL)],
                        rows: &[
                            &[T(r#"br1"#), T(r#"f"#)],
                            &[T(r#"br2"#), T(r#"t"#)],
                            &[T(r#"br3"#), T(r#"t"#)],
                            &[T(r#"main"#), T(r#"f"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('br1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'br1'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT name, dirty FROM dolt_branches ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("dirty", BOOL)],
                        rows: &[
                            &[T(r#"br1"#), T(r#"f"#)],
                            &[T(r#"br2"#), T(r#"t"#)],
                            &[T(r#"br3"#), T(r#"t"#)],
                            &[T(r#"main"#), T(r#"f"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: create database errors"#,
            intent: r#"Creating a database that exists fails."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test1;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test1;"#,
                    expected: Expected::Error(Diagnostic { code: "42P04", message: r#"database "test1" already exists"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI's checks for files and directories of the new database's name are its own"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: can create savepoint when no database is selected"#,
            intent: r#"A transaction can take a savepoint at once."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SAVEPOINT tx1;"#,
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMIT;"#,
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: manual commit table can be dropped (validates superschema structure)"#,
            intent: r#"A committed table can be dropped and the drop committed."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Already on branch 'main'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE one_pk (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO one_pk (pk, c1, c2) VALUES (2, 2, 2), (3, 3, 3);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'test commit message', '--author', 'John Doe <john@example.com>');"#,
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
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"one_pk"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE one_pk;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    client: "other",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "other",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'Dropped table one_pk');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "other",
                    database: "postgres",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
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
            name: r#"sql-server: create database, drop it, and then create it again"#,
            intent: r#"A dropped database's name can be used again."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE mydb1;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE mydb1;"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE mydb1;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname = 'mydb1';"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(r#"mydb1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: dropping database with '-' in it"#,
            intent: r#"A database whose name holds a hyphen can be dropped."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "my-db";"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname = 'my-db';"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(r#"my-db"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE "my-db";"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname = 'my-db';"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
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
            name: r#"sql-server: dolt_clone procedure in empty dir"#,
            intent: r#"dolt_clone makes a database with the remote's tables and routines."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION test() RETURNS INT LANGUAGE SQL AS $$ SELECT 42 $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's procedure returning a result set is a Postgres function"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'initial commit');"#,
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
                    query: r#"SELECT dolt_remote('add', 'remote1', 'file://{NEWDIR:rem1}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('remote1', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:rem1}', 'repo3');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT current_database();"#,
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T(r#"repo3"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo3",
                    database: "repo3",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                            &[T(r#"3"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "repo3",
                    database: "repo3",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT test();"#,
                    expected: Expected::Rows {
                        columns: &[Column("test", INT4)],
                        rows: &[
                            &[T(r#"42"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "repo3",
                    database: "repo3",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: server with no dbs yet should be able to clone"#,
            intent: r#"A database made on a fresh server can clone a remote."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'remote01', 'file://{NEWDIR:remote}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_push('remote01', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test01;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:remote}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test01",
                    database: "test01",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname = 'remote';"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(r#"remote"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: test multi db without use statements"#,
            intent: r#"Tables of two databases are written on their own connections and read through each database's name."#,
            set_up_script: &[
                r#"CREATE DATABASE repo1;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE repo2;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r1_one_pk (pk BIGINT NOT NULL, c1 BIGINT, c2 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo1",
                    database: "repo1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE r2_one_pk (pk BIGINT NOT NULL, c3 BIGINT, c4 BIGINT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r1_one_pk (pk, c1, c2) VALUES (2, 2, 2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo1",
                    database: "repo1",
                    dolt: Some(Dolt::Differs(r#"Doltgrust reads other databases' tables read-only (STORAGE_DESIGN section V.3), so the port writes each table on a connection to its own database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO r2_one_pk (pk, c3, c4) VALUES (2, 2, 2);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM repo1.public.r1_one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T(r#"2"#), T(r#"2"#), T(r#"2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some(r#"reading another database's table as db.schema.table (STORAGE_DESIGN section V.3) is designed but not built, failing with 0A000 cross-database references are not implemented"#),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM repo2.public.r2_one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c3", INT8), Column("c4", INT8)],
                        rows: &[
                            &[T(r#"2"#), T(r#"2"#), T(r#"2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some(r#"reading another database's table as db.schema.table (STORAGE_DESIGN section V.3) is designed but not built, failing with 0A000 cross-database references are not implemented"#),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: Run queries on database without ever selecting it"#,
            intent: r#"Another database's table is read through its name after its own connection writes it."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE repo2;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE one_pk (pk INT, PRIMARY KEY (pk));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "repo2",
                    database: "repo2",
                    dolt: Some(Dolt::Differs(r#"Doltgrust reads other databases' tables read-only (STORAGE_DESIGN section V.3), so the port writes each table on a connection to its own database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO one_pk VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE one_pk SET pk = 3 WHERE pk = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM one_pk WHERE pk = 3;"#,
                    expected: Expected::Tag("DELETE 1"),
                    client: "repo2",
                    database: "repo2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM repo2.public.one_pk ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"0"#)],
                            &[T(r#"1"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    skip: Some(r#"reading another database's table as db.schema.table (STORAGE_DESIGN section V.3) is designed but not built, failing with 0A000 cross-database references are not implemented"#),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql-server: create database without USE"#,
            intent: r#"A table made in a new database is visible there and through the database's name."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE newdb;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (a INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "newdb",
                    database: "newdb",
                    dolt: Some(Dolt::Differs(r#"Doltgrust reads other databases' tables read-only (STORAGE_DESIGN section V.3), so the port writes each table on a connection to its own database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"test"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "newdb",
                    database: "newdb",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM newdb.public.test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some(r#"reading another database's table as db.schema.table (STORAGE_DESIGN section V.3) is designed but not built, failing with 0A000 cross-database references are not implemented"#),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
