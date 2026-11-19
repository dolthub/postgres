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

//! Dolt's integration-tests/bats/sql-privs.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_privs() {
    run_scripts(&[
        ScriptTest {
            name: "sql-privs: basic lack of privileges tests",
            intent: "A role reads a table while it holds SELECT and is refused once it is revoked, and the catalog lists its grants.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1(c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER test PASSWORD 'test';",
                    expected: Expected::Tag("CREATE ROLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CREATE USER without a password lets the user log in, where a Postgres role logs in here with a password")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO test;",
                    expected: Expected::Tag("GRANT"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's grant on every table of a database is GRANT ... ON ALL TABLES IN SCHEMA, and its user@host accounts are roles")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "test",
                    password: "test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grantee, table_name, privilege_type FROM information_schema.role_table_grants WHERE grantee = 'test' ORDER BY 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("grantee", NAME), Column("table_name", NAME), Column("privilege_type", VARCHAR)],
                        rows: &[
                            &[T("test"), T("t1"), T("SELECT")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("information_schema's SCHEMA_PRIVILEGES, TABLE_PRIVILEGES, and USER_PRIVILEGES are MySQL's, so role_table_grants lists the role's privileges")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON ALL TABLES IN SCHEMA public FROM test;",
                    expected: Expected::Tag("REVOKE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's grant on every table of a database is GRANT ... ON ALL TABLES IN SCHEMA, and its user@host accounts are roles")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table t1", ..E }),
                    username: "test",
                    password: "test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE ON t1 TO test;",
                    expected: Expected::Tag("GRANT"),
                    dolt: Some(Dolt::MySqlOnly("the test's later accounts by host are MySQL's, so the grant goes to the same role")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grantee, table_name, privilege_type FROM information_schema.role_table_grants WHERE grantee = 'test' ORDER BY 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("grantee", NAME), Column("table_name", NAME), Column("privilege_type", VARCHAR)],
                        rows: &[
                            &[T("test"), T("t1"), T("UPDATE")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("information_schema's SCHEMA_PRIVILEGES, TABLE_PRIVILEGES, and USER_PRIVILEGES are MySQL's, so role_table_grants lists the role's privileges")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-privs: creating user identified by password",
            intent: "A role created with a password logs in with it and not with another.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE USER test PASSWORD 'test';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "test",
                    password: "test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Error(Diagnostic { severity: "FATAL", code: "28P01", message: "password authentication failed for user \"test\"", ..E }),
                    username: "test",
                    password: "bad",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-privs: deleting user prevents access by that user",
            intent: "A dropped role can no longer log in.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1(c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER test PASSWORD 'test';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON ALL TABLES IN SCHEMA public TO test;",
                    expected: Expected::Tag("GRANT"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's grant on every table of a database is GRANT ... ON ALL TABLES IN SCHEMA, and its user@host accounts are roles")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("c1", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    username: "test",
                    password: "test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY test;",
                    expected: Expected::Tag("DROP OWNED"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's DROP USER drops the user's privileges with it, where Postgres drops them with DROP OWNED first")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP USER test;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Error(Diagnostic { severity: "FATAL", code: "28P01", message: "password authentication failed for user \"test\"", ..E }),
                    username: "test",
                    password: "test",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-privs: revoking last privilege doesn't result in corrupted privileges file",
            intent: "Revoking a role's privileges one at a time leaves exactly the ones still granted.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL grants privileges on a database before it has tables, so the port grants them on a table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER tester PASSWORD 'tester';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON t TO tester;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON t FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grantee, table_name, privilege_type FROM information_schema.role_table_grants WHERE grantee = 'tester' ORDER BY 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("grantee", NAME), Column("table_name", NAME), Column("privilege_type", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW GRANTS is MySQL's, so role_table_grants lists the role's privileges")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON t TO tester;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT UPDATE ON t TO tester;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grantee, table_name, privilege_type FROM information_schema.role_table_grants WHERE grantee = 'tester' ORDER BY 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("grantee", NAME), Column("table_name", NAME), Column("privilege_type", VARCHAR)],
                        rows: &[
                            &[T("tester"), T("t"), T("SELECT")],
                            &[T("tester"), T("t"), T("UPDATE")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW GRANTS is MySQL's, so role_table_grants lists the role's privileges")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE UPDATE ON t FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grantee, table_name, privilege_type FROM information_schema.role_table_grants WHERE grantee = 'tester' ORDER BY 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("grantee", NAME), Column("table_name", NAME), Column("privilege_type", VARCHAR)],
                        rows: &[
                            &[T("tester"), T("t"), T("SELECT")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW GRANTS is MySQL's, so role_table_grants lists the role's privileges")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-privs: revoking all privileges doesn't result in a corrupted privileges file",
            intent: "REVOKE ALL leaves a role no privileges on the table.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL grants privileges on a database before it has tables, so the port grants them on a table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER tester PASSWORD 'tester';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON t TO tester;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALL ON t FROM tester;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grantee, table_name, privilege_type FROM information_schema.role_table_grants WHERE grantee = 'tester' ORDER BY 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("grantee", NAME), Column("table_name", NAME), Column("privilege_type", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW GRANTS is MySQL's, so role_table_grants lists the role's privileges")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
