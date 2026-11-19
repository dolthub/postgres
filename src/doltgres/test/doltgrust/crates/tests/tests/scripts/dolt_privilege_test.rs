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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_privilege_test.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_only_revision_db_privilege_tests() {
    run_scripts(&[
        ScriptTest {
            name: "Basic database and table name visibility",
            intent: "A user reads a branch's table only with SELECT on it, granted directly or through a role, on that branch.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
                "CREATE ROLE test_role;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO test_role;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL hides a table the user can't read behind its access error, and Postgres reports the missing table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON ALL TABLES IN SCHEMA public FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    client: "root_b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL hides a table the user can't read behind its access error, and Postgres reports the missing table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM public.test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON ALL TABLES IN SCHEMA public FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    client: "root_b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL hides a table the user can't read behind its access error, and Postgres reports the missing table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON test TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL hides a table the user can't read behind its access error, and Postgres reports the missing table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON test FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    client: "root_b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL hides a table the user can't read behind its access error, and Postgres reports the missing table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON test2 TO tester;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", ..E }),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a table that doesn't exist, and Postgres refuses")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON test2 FROM tester;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", ..E }),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a table that doesn't exist, and Postgres refuses")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL hides a table the user can't read behind its access error, and Postgres reports the missing table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT test_role TO tester;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic SELECT and INSERT privilege checking",
            intent: "INSERT on a branch's table needs the INSERT privilege on that branch, and reading it, a WHERE clause's columns included, needs SELECT.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT INSERT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "root_b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic UPDATE privilege checking",
            intent: "UPDATE on a branch's table needs the UPDATE privilege on that branch, and reading it, a WHERE clause's columns included, needs SELECT.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE test SET pk = 4 WHERE pk = 3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET pk = 4 WHERE pk = 3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL lets a statement read the columns its WHERE clause names without SELECT, and Postgres needs SELECT on them")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "root_b1",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic DELETE privilege checking",
            intent: "DELETE on a branch's table needs the DELETE privilege on that branch, and reading it, a WHERE clause's columns included, needs SELECT.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT DELETE ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk = 3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL lets a statement read the columns its WHERE clause names without SELECT, and Postgres needs SELECT on them")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "root_b1",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic CREATE TABLE privilege checking",
            intent: "Creating a table on a branch needs CREATE on its schema there.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (a int PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", position: 14, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL's CREATE on a database is Postgres' CREATE on the schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (a int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t2")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES is MySQL's and lists every table, and information_schema lists the tables the user has a privilege on")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic DROP TABLE privilege checking",
            intent: "Dropping a branch's table takes its ownership, which membership in the owner role gives.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "CREATE ROLE test_owner;",
                "ALTER TABLE test OWNER TO test_owner;",
                "GRANT CREATE ON SCHEMA public TO test_owner;",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT test_owner TO tester;",
                    expected: Expected::Tag("GRANT ROLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no DROP, ALTER, or INDEX privilege, only a table's owner, so the port grants membership in the tables' owner role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' AND table_name = 'test';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES and DESCRIBE are MySQL's, so the port reads information_schema")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic ALTER TABLE privilege checking",
            intent: "Altering a branch's table takes its ownership, which membership in the owner role gives.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "CREATE ROLE test_owner;",
                "ALTER TABLE test OWNER TO test_owner;",
                "GRANT CREATE ON SCHEMA public TO test_owner;",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD COLUMN a int;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT test_owner TO tester;",
                    expected: Expected::Tag("GRANT ROLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no DROP, ALTER, or INDEX privilege, only a table's owner, so the port grants membership in the tables' owner role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD COLUMN a int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                            &[T("a"), T("integer")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES and DESCRIBE are MySQL's, so the port reads information_schema")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic INDEX privilege checking",
            intent: "Creating and dropping an index on a branch's table takes the table's ownership.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY, a int);",
                "INSERT INTO test VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE ROLE test_owner;",
                "ALTER TABLE test OWNER TO test_owner;",
                "GRANT CREATE ON SCHEMA public TO test_owner;",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX t1 ON test (a);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t1 ON test (a);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT test_owner TO tester;",
                    expected: Expected::Tag("GRANT ROLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no DROP, ALTER, or INDEX privilege, only a table's owner, so the port grants membership in the tables' owner role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t1 ON test (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY indexname;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("t1")],
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES and DESCRIBE are MySQL's, so the port reads information_schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE test_owner FROM tester;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no DROP, ALTER, or INDEX privilege, only a table's owner, so the port grants membership in the tables' owner role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX t1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of index t1", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT test_owner TO tester;",
                    expected: Expected::Tag("GRANT ROLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no DROP, ALTER, or INDEX privilege, only a table's owner, so the port grants membership in the tables' owner role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX t1;",
                    expected: Expected::Tag("DROP INDEX"),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 'test' ORDER BY indexname;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("test_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("SHOW TABLES and DESCRIBE are MySQL's, so the port reads information_schema")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic constraint privilege checking",
            intent: "Adding and dropping a check constraint on a branch's table takes the table's ownership.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY, a int);",
                "INSERT INTO test VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE ROLE test_owner;",
                "ALTER TABLE test OWNER TO test_owner;",
                "GRANT CREATE ON SCHEMA public TO test_owner;",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD CONSTRAINT chk0 CHECK (NULL = NULL);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD CONSTRAINT chk0 CHECK (NULL = NULL);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT test_owner TO tester;",
                    expected: Expected::Tag("GRANT ROLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no DROP, ALTER, or INDEX privilege, only a table's owner, so the port grants membership in the tables' owner role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD CONSTRAINT chk1 CHECK (a < 10);",
                    expected: Expected::Tag("ALTER TABLE"),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'test'::regclass AND contype = 'c' ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("chk1"), T("CHECK ((a < 10))")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so the port reads pg_constraint")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE test_owner FROM tester;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no DROP, ALTER, or INDEX privilege, only a table's owner, so the port grants membership in the tables' owner role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT chk1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT test_owner TO tester;",
                    expected: Expected::Tag("GRANT ROLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no DROP, ALTER, or INDEX privilege, only a table's owner, so the port grants membership in the tables' owner role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT chk1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'test'::regclass AND contype = 'c' ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic revoke SELECT privilege",
            intent: "Revoking SELECT on a branch takes away reading its table.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname FROM pg_roles WHERE rolname = 'tester';",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME)],
                        rows: &[
                            &[T("tester")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("mysql.user and mysql.role_edges are MySQL's, so the port reads pg_roles and pg_auth_members")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON ALL TABLES IN SCHEMA public FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    client: "root_b1",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname FROM pg_roles WHERE rolname = 'tester';",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME)],
                        rows: &[
                            &[T("tester")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("mysql.user and mysql.role_edges are MySQL's, so the port reads pg_roles and pg_auth_members")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Grant Role with SELECT Privilege",
            intent: "A role's SELECT on a branch's table reaches its members.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
                "CREATE ROLE test_role;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO test_role;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname, m.rolname FROM pg_auth_members a JOIN pg_roles r ON r.oid = a.roleid JOIN pg_roles m ON m.oid = a.member WHERE r.rolname = 'test_role';",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("mysql.user and mysql.role_edges are MySQL's, so the port reads pg_roles and pg_auth_members")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT test_role TO tester;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname, m.rolname FROM pg_auth_members a JOIN pg_roles r ON r.oid = a.roleid JOIN pg_roles m ON m.oid = a.member WHERE r.rolname = 'test_role';",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolname", NAME)],
                        rows: &[
                            &[T("test_role"), T("tester")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("mysql.user and mysql.role_edges are MySQL's, so the port reads pg_roles and pg_auth_members")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Revoke role currently granted to a user",
            intent: "Revoking a role takes away the privileges its members had through it, and leaves both roles.",
            set_up_script: &[
                "CREATE TABLE test (pk BIGINT PRIMARY KEY);",
                "INSERT INTO test VALUES (1), (2), (3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE USER tester PASSWORD 'tester';",
                "CREATE ROLE test_role;",
                "GRANT test_role TO tester;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO test_role;",
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname, m.rolname FROM pg_auth_members a JOIN pg_roles r ON r.oid = a.roleid JOIN pg_roles m ON m.oid = a.member WHERE r.rolname = 'test_role';",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolname", NAME)],
                        rows: &[
                            &[T("test_role"), T("tester")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("mysql.user and mysql.role_edges are MySQL's, so the port reads pg_roles and pg_auth_members")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE test_role FROM tester;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table test", ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname, m.rolname FROM pg_auth_members a JOIN pg_roles r ON r.oid = a.roleid JOIN pg_roles m ON m.oid = a.member WHERE r.rolname = 'test_role';",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("mysql.user and mysql.role_edges are MySQL's, so the port reads pg_roles and pg_auth_members")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_roles WHERE rolname IN ('test_role', 'tester');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("mysql.user and mysql.role_edges are MySQL's, so the port reads pg_roles and pg_auth_members")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_only_revision_table_function_privilege_tests() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_schema_diff privilege checking with revision database",
            intent: "dolt_schema_diff on a branch connection shows a table's schema change only to a user who may read that table, and every table's only to a user who may read them all.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"CREATE TABLE test2 (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"ALTER TABLE test RENAME COLUMN col1 TO word;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON test TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    col1 character varying(20)\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    word character varying(20)\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);\n\nGRANT SELECT ON TABLE public.test TO tester;")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("the CREATE TABLE statements are Postgres', in pg_dump's form, and the grant is part of the branch's schema, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL refuses a user without SELECT on every table of the database as a whole, and Postgres refuses at the first table the user may not read")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    col1 character varying(20)\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    word character varying(20)\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);\n\nGRANT SELECT ON TABLE public.test TO tester;")],
                            &[T("public.test2"), T("public.test2"), T("CREATE TABLE public.test2 (\n    pk bigint NOT NULL,\n    col1 character varying(20)\n);\n\nALTER TABLE public.test2 OWNER TO postgres;\n\nALTER TABLE ONLY public.test2\n    ADD CONSTRAINT test2_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test2 (\n    pk bigint NOT NULL,\n    col1 character varying(20)\n);\n\nALTER TABLE public.test2 OWNER TO postgres;\n\nALTER TABLE ONLY public.test2\n    ADD CONSTRAINT test2_pkey PRIMARY KEY (pk);\n\nGRANT SELECT ON TABLE public.test2 TO tester;")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("the CREATE TABLE statements are Postgres', in pg_dump's form, and the grant is part of the branch's schema, Appendix C \"roles\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff privilege checking with revision database",
            intent: "dolt_diff on a branch connection needs SELECT on the table it diffs.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT8), Column("to_col1", VARCHAR), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_pk", INT8), Column("from_col1", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff_stat privilege checking with revision database",
            intent: "dolt_diff_stat on a branch connection needs SELECT on the table it reads, or on every table when it names none.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_stat('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_stat('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_stat('HEAD', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff_summary privilege checking with revision database",
            intent: "dolt_diff_summary on a branch connection needs SELECT on the table it reads, or on every table when it names none.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Doltgrust's object privileges are branch data, so the grant made through the branch connection is a schema change of its working set, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Doltgrust's object privileges are branch data, so the grant made through the branch connection is a schema change of its working set, Appendix C \"roles\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_merge_bases privilege checking with revision database",
            intent: "dolt_merge_bases on a branch connection needs SELECT on every table.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"CREATE TABLE test2 (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_merge_bases('main', 'b1');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_merge_bases succeeds for a role the test expects it to refuse (42501), as the other revision table functions refuse it (bug, MacDG)"),
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON test TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_merge_bases('main', 'b1');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_merge_bases succeeds for a role the test expects it to refuse (42501), as the other revision table functions refuse it (bug, MacDG)"),
                    dolt: Some(Dolt::MySqlOnly("MySQL refuses a user without SELECT on every table of the database as a whole, and Postgres refuses at the first table the user may not read")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_merge_bases('main', 'b1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_log privilege checking with revision database",
            intent: "dolt_log on a branch connection needs SELECT on every table, even when it filters by one table.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"CREATE TABLE test2 (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log();"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON test TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log();"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL refuses a user without SELECT on every table of the database as a whole, and Postgres refuses at the first table the user may not read")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log('main', '--tables', 'test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL refuses a user without SELECT on every table of the database as a whole, and Postgres refuses at the first table the user may not read")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log();"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log('main', '--tables', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_patch privilege checking with revision database",
            intent: "dolt_patch on a branch connection needs SELECT on the table it reads, or on every table when it names none.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD', 'WORKING');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("HEAD"), T("WORKING"), T("public.test"), T("schema"), T("GRANT SELECT ON TABLE public.test TO tester;")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Doltgrust's object privileges are branch data, so the grant made through the branch connection is a schema change of its working set, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_patch('HEAD', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", INT8), Column("from_commit_hash", TEXT), Column("to_commit_hash", TEXT), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("HEAD"), T("WORKING"), T("public.test"), T("schema"), T("GRANT SELECT ON TABLE public.test TO tester;")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Doltgrust's object privileges are branch data, so the grant made through the branch connection is a schema change of its working set, Appendix C \"roles\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_preview_merge_conflicts privilege checking with revision database",
            intent: "dolt_preview_merge_conflicts on a branch connection needs SELECT on the table whose conflicts it previews.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"CREATE TABLE test2 (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_preview_merge_conflicts('main', 'b1', 'test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON test TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_preview_merge_conflicts('main', 'b1', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_from_root_ish", TEXT), Column("base_pk", INT8), Column("base_col1", VARCHAR), Column("our_pk", INT8), Column("our_col1", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT8), Column("their_col1", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("dolt_conflict_id", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_preview_merge_conflicts('main', 'b1', 'test2');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL refuses a user without SELECT on every table of the database as a whole, and Postgres refuses at the first table the user may not read")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_preview_merge_conflicts_summary privilege checking with revision database",
            intent: "dolt_preview_merge_conflicts_summary on a branch connection needs SELECT on every table.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"CREATE TABLE test2 (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'b1');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON test TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'b1');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("MySQL refuses a user without SELECT on every table of the database as a whole, and Postgres refuses at the first table the user may not read")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'b1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_query_diff privilege checking with revision database",
            intent: "dolt_query_diff on a branch connection runs its queries with the user's privileges, so it needs SELECT on every table they read and nothing for a query that reads none.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"CREATE TABLE test2 (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test2 VALUES (1, 'a'), (2, 'b');"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM test', 'SELECT pk FROM test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT t.pk FROM test t JOIN test2 t2 ON t.pk = t2.pk', 'SELECT t.pk FROM test t JOIN test2 t2 ON t.pk = t2.pk');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    dolt: Some(Dolt::MySqlOnly("MySQL grants on a database's tables as a whole and refuses a user without them at the database, and Postgres grants on tables and refuses at the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT 1 AS pk', 'SELECT 2 AS pk');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("to_pk", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    dolt: Some(Dolt::MySqlOnly("MySQL refuses a user without any privilege on the database at the database, and Postgres lets anyone run a query that reads no table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON test TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM test', 'SELECT pk FROM test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT 1 AS pk', 'SELECT 2 AS pk');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("to_pk", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT t.pk FROM test t JOIN test2 t2 ON t.pk = t2.pk', 'SELECT t.pk FROM test t JOIN test2 t2 ON t.pk = t2.pk');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM (SELECT pk FROM test2) s', 'SELECT pk FROM (SELECT pk FROM test) s');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON ALL TABLES IN SCHEMA public TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT t.pk FROM test t JOIN test2 t2 ON t.pk = t2.pk', 'SELECT t.pk FROM test t JOIN test2 t2 ON t.pk = 2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT 1 AS pk', 'SELECT 2 AS pk');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("to_pk", INT4), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM test limit 1', 'SELECT pk FROM test2 limit 1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, T("deleted")],
                            &[Null, T("1"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM (SELECT pk FROM test2) s', 'SELECT pk FROM (SELECT pk FROM test) s');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_query_diff privilege checking with qualified table names",
            intent: "dolt_query_diff checks the privileges of every table its queries name, however they qualify it, including tables of other schemas and those an EXPLAIN only plans.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"CREATE TABLE test2 (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test2 VALUES (1, 'a'), (2, 'b');"#,
                r#"CREATE SCHEMA otherdb;"#,
                r#"CREATE TABLE otherdb.other_table (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO otherdb.other_table VALUES (1, 'other1'), (2, 'other2');"#,
                r#"CREATE TABLE otherdb.other_table2 (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO otherdb.other_table2 VALUES (1, 'other3'), (2, 'other4');"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"GRANT SELECT ON test TO tester;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM public.test', 'SELECT pk FROM public.test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM public.test UNION ALL SELECT pk FROM public.test2', 'SELECT pk FROM public.test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM otherdb.other_table', 'SELECT pk FROM otherdb.other_table');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for schema otherdb"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's other database is a schema in Postgres, where a query reaches another database only through a revision-qualified name, and the schema needs USAGE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM public.test', 'SELECT pk FROM test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's mydb.test from a mydb/b1 connection reads the default branch, and Postgres' public.test reads the connection's own branch, so both queries read the same rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT t.pk FROM public.test t JOIN public.test2 t2 ON t.pk = t2.pk', 'SELECT t.pk FROM public.test t JOIN public.test2 t2 ON t.pk = 2');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table test2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON test2 TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT t.pk FROM public.test t JOIN public.test2 t2 ON t.pk = t2.pk', 'SELECT t.pk FROM public.test t JOIN public.test2 t2 ON t.pk = 2');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT * FROM dolt_query_diff(''SELECT pk FROM public.test'', ''SELECT pk FROM public.test'')', 'SELECT * FROM dolt_query_diff(''SELECT pk FROM public.test2'', ''SELECT pk FROM public.test2'')');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_from_pk", INT8), Column("from_to_pk", INT8), Column("from_diff_type", TEXT), Column("to_from_pk", INT8), Column("to_to_pk", INT8), Column("to_diff_type", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT USAGE ON SCHEMA otherdb TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON otherdb.other_table TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk, col1 FROM otherdb.other_table', 'SELECT pk, col1 FROM public.test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("from_col1", VARCHAR), Column("to_pk", INT8), Column("to_col1", VARCHAR), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("other1"), Null, Null, T("deleted")],
                            &[T("2"), T("other2"), Null, Null, T("deleted")],
                            &[Null, Null, T("1"), T("first row"), T("added")],
                            &[Null, Null, T("2"), T("second row"), T("added")],
                        ],
                        tag: "SELECT 4",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXPLAIN SELECT * FROM dolt_query_diff('SELECT pk FROM otherdb.other_table2', 'SELECT pk FROM otherdb.other_table2');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table other_table2"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXPLAIN (COSTS OFF) SELECT pk FROM public.test UNION ALL SELECT pk FROM public.test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Seq Scan on test")],
                            &[T("  ->  Seq Scan on test test_1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    dolt: Some(Dolt::MySqlOnly("the plan is Postgres', not go-mysql-server's")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXPLAIN (COSTS OFF) SELECT * FROM dolt_query_diff('SELECT pk, (col1::json ->> ''a'') FROM public.test', 'SELECT pk, (col1::json ->> ''a'') FROM public.test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on dolt_query_diff")],
                        ],
                        tag: "EXPLAIN",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    dolt: Some(Dolt::MySqlOnly("the plan is Postgres', not go-mysql-server's, and JSON_EXTRACT is ->> on a json cast")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk, (col1::json ->> ''a'') FROM public.test', 'SELECT pk, (col1::json ->> ''a'') FROM public.test');"#,
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type json"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_query_diff view privilege checking",
            intent: "dolt_query_diff runs a query over a view with the privileges Postgres gives views, so a user needs SELECT on the view and not on its tables.",
            set_up_script: &[
                r#"CREATE TABLE test (pk BIGINT PRIMARY KEY, col1 varchar(20));"#,
                r#"INSERT INTO test VALUES (1, 'first row'), (2, 'second row');"#,
                r#"CREATE VIEW v_test AS SELECT pk, col1 FROM test;"#,
                r#"SELECT dolt_commit('-Am', 'first commit');"#,
                r#"CREATE USER tester PASSWORD 'tester';"#,
                r#"SELECT dolt_branch('b1');"#,
                r#"SELECT * FROM dolt_checkout('b1');"#,
                r#"GRANT SELECT ON v_test TO tester;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM v_test', 'SELECT pk FROM v_test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    dolt: Some(Dolt::MySqlOnly("Postgres reads a view's tables with its owner's privileges, so SELECT on the view is enough, where Dolt also checks the user's privileges on the view's tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT ON test TO tester;"#,
                    expected: Expected::Tag("GRANT"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM v_test', 'SELECT pk FROM v_test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"REVOKE SELECT ON v_test FROM tester;"#,
                    expected: Expected::Tag("REVOKE"),
                    client: "root_b1",
                    database: "postgres/b1",
                    dolt: Some(Dolt::Differs("Dolt's grants are server-wide, and Doltgrust's object privileges are branch data, so the grant goes through a connection to the branch, Appendix C \"roles\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM v_test', 'SELECT pk FROM v_test');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for view v_test"#, ..E }),
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_query_diff('SELECT pk FROM test', 'SELECT pk FROM test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("to_pk", INT8), Column("diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "tester",
                    password: "tester",
                    client: "tester",
                    database: "postgres/b1",
                    skip: Some("dolt_query_diff isn't built (REBUILD_PLAN, dolt_query_diff and dolt_json_diff)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
