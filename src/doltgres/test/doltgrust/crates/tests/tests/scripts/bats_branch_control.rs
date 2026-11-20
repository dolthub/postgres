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

//! Dolt's integration-tests/bats/branch-control.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_branch_control() {
    run_scripts(&[
        ScriptTest {
            name: "branch-control: fresh database. branch control tables exist",
            intent: "A new database has branch control's two tables, the permission rules holding only the default row that lets everyone write every branch.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("%"), T("%"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_namespace_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT column_name FROM information_schema.columns WHERE table_name = 'dolt_branch_control' ORDER BY ordinal_position;"#,
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("database")],
                            &[T("branch")],
                            &[T("user")],
                            &[T("host")],
                            &[T("permissions")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so the port reads the view's columns from information_schema.columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT column_name FROM information_schema.columns WHERE table_name = 'dolt_branch_namespace_control' ORDER BY ordinal_position;"#,
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("database")],
                            &[T("branch")],
                            &[T("user")],
                            &[T("host")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so the port reads the view's columns from information_schema.columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: fresh database. branch control tables exist through server interface",
            intent: "A server's clients see the same default branch control rows.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("%"), T("%"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_namespace_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT)],
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
            name: "branch-control: modify dolt_branch_control from dolt sql then make sure changes are reflected",
            intent: "A rule written to dolt_branch_control is what a later session reads back.",
            database: "test_db",
            set_up_script: &[
                "CREATE USER test PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test;",
                "SELECT dolt_commit('-Am', 'users');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_branch_control VALUES ('test_db','test-branch', 'test', '%', 'write');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("test_db"), T("test-branch"), T("test"), T("%"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("test_db"), T("test-branch"), T("test"), T("%"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "server",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: default user root works as expected",
            intent: "The superuser can delete the default rule, leaving no rules.",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("%"), T("%"), T("write")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_branch_control WHERE "user" = '%';"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
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
            name: "branch-control: test basic branch write permissions",
            intent: "A user with write on one branch writes only there, and whoever creates a branch may write it.",
            set_up_script: &[
                "CREATE USER test PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test;",
                "SELECT dolt_commit('-Am', 'users');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('postgres', 'test-branch', 'test', '%', 'write');",
                "SELECT dolt_branch('test-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (c1 int);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (c1 int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    username: "test",
                    password: "password",
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    client: "second",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('t');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    client: "second",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'Testing commit');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    client: "second",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'test-branch-2');"#,
                    expected: Expected::Ok,
                    client: "third",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (c1 int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "third",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (c1 int);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`postgres`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: test admin permissions",
            intent: "An admin of a branch writes it and edits its rules, granting and taking back another user's write, but gets no write on other branches.",
            set_up_script: &[
                "CREATE USER test PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test;",
                "CREATE USER test2 PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test2;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test2;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test2;",
                "ALTER DEFAULT PRIVILEGES FOR ROLE test IN SCHEMA public GRANT ALL ON TABLES TO test2;",
                "SELECT dolt_commit('-Am', 'users');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('postgres', 'test-branch', 'test', '%', 'admin');",
                "SELECT dolt_branch('test-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (c1 int);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`test`@`localhost` does not have the correct permissions on branch `main`"#, ..E }),
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (c1 int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    username: "test",
                    password: "password",
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_branch_control VALUES ('postgres', 'test-branch', 'test2', '%', 'write');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("postgres"), T("test-branch"), T("test"), T("%"), T("admin")],
                            &[T("postgres"), T("test-branch"), T("postgres"), T("localhost"), T("admin")],
                            &[T("postgres"), T("test-branch"), T("test2"), T("%"), T("write")],
                        ],
                        tag: "SELECT 3",
                    },
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("postgres"), T("test-branch"), T("test"), T("%"), T("admin")],
                            &[T("postgres"), T("test-branch"), T("postgres"), T("localhost"), T("admin")],
                            &[T("postgres"), T("test-branch"), T("test2"), T("%"), T("write")],
                        ],
                        tag: "SELECT 3",
                    },
                    username: "test2",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test2",
                    password: "password",
                    client: "second",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (0);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    username: "test2",
                    password: "password",
                    client: "second",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_branch_control WHERE "user" = 'test2';"#,
                    expected: Expected::Tag("DELETE 1"),
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("postgres"), T("test-branch"), T("test"), T("%"), T("admin")],
                            &[T("postgres"), T("test-branch"), T("postgres"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test2",
                    password: "password",
                    client: "third",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`test2`@`localhost` does not have the correct permissions on branch `test-branch`"#, ..E }),
                    username: "test2",
                    password: "password",
                    client: "third",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: creating a branch grants admin permissions",
            intent: "A user who creates a branch becomes its admin.",
            set_up_script: &[
                "CREATE USER test PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test;",
                "SELECT dolt_commit('-Am', 'users');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('postgres', 'main', 'test', '%', 'write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("postgres"), T("main"), T("test"), T("%"), T("write")],
                            &[T("postgres"), T("test-branch"), T("test"), T("localhost"), T("admin")],
                        ],
                        tag: "SELECT 2",
                    },
                    username: "test",
                    password: "password",
                    dolt: Some(Dolt::Differs("A Postgres role has no host, so the admin rule of a branch's creator names the host its session connected from, localhost here, where Dolt names the host of the MySQL account, % (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: test branch namespace control",
            intent: "A namespace rule lets only the users it names create branches whose names it matches.",
            set_up_script: &[
                "CREATE USER test PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test;",
                "CREATE USER test2 PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test2;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test2;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test2;",
                "SELECT dolt_commit('-Am', 'users');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('postgres', 'test-
branch', 'test', '%', 'admin');",
                "INSERT INTO dolt_branch_namespace_control VALUES ('postgres', 'test-%', 'test2', '%');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_namespace_control;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT)],
                        rows: &[
                            &[T("postgres"), T("test-%"), T("test2"), T("%")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('test-branch');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`test`@`localhost` cannot create a branch named `test-branch`"#, ..E }),
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test2",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: test longest match in branch namespace control",
            intent: "Each user creates branches only in the namespace whose rule names them.",
            set_up_script: &[
                "CREATE USER test PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test;",
                "CREATE USER test2 PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test2;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test2;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test2;",
                "SELECT dolt_commit('-Am', 'users');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_namespace_control VALUES ('postgres', 'test/%', 'test', '%');",
                "INSERT INTO dolt_branch_namespace_control VALUES ('postgres', 'test2/%', 'test2', '%');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('test/branch1');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('test2/branch1');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`test`@`localhost` cannot create a branch named `test2/branch1`"#, ..E }),
                    username: "test",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('test2/branch1');"#,
                    expected: Expected::Ok,
                    username: "test2",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('test/branch1');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`test2`@`localhost` cannot create a branch named `test/branch1`"#, ..E }),
                    username: "test2",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: test longest match in branch access control",
            intent: "The longest matching rule decides a user's permissions, so a narrower read rule overrides a wider write rule until it is deleted.",
            set_up_script: &[
                "CREATE USER test PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO test;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO test;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO test;",
                "CREATE USER admin PASSWORD 'password';",
                "GRANT ALL ON SCHEMA public TO admin;",
                "GRANT ALL ON ALL TABLES IN SCHEMA public TO admin;",
                "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES TO admin;",
                "SELECT dolt_commit('-Am', 'users');",
                "DELETE FROM dolt_branch_control WHERE \"user\" = '%';",
                "INSERT INTO dolt_branch_control VALUES ('%', '%', 'admin', '%', 'admin');",
                "INSERT INTO dolt_branch_control VALUES ('postgres', 'test-branch', 'test', '%', 'read');",
                "INSERT INTO dolt_branch_control VALUES ('postgres', '%', 'test', '%', 'write');",
                "SELECT dolt_branch('test-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (c1 int);"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`test`@`localhost` does not have the correct permissions on branch `test-branch`"#, ..E }),
                    username: "test",
                    password: "password",
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_branch_control WHERE branch = 'test-branch';"#,
                    expected: Expected::Tag("DELETE 2"),
                    username: "admin",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('test-branch');"#,
                    expected: Expected::Ok,
                    username: "test",
                    password: "password",
                    client: "second",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (c1 int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    username: "test",
                    password: "password",
                    client: "second",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: repeat deletion does not cause a nil panic",
            intent: "Deleting every rule and adding them again, several times over, leaves exactly the rules added last, and deleting them all leaves none.",
            database: "dolt",
            set_up_script: &[
                "DELETE FROM dolt_branch_control;",
                "INSERT INTO dolt_branch_control VALUES ('dolt','s1','ab','%','admin');",
                "INSERT INTO dolt_branch_control VALUES ('dolt','s2','ab','%','admin');",
                "INSERT INTO dolt_branch_control VALUES ('%','%','%','%','write');",
                "DELETE FROM dolt_branch_control;",
                "INSERT INTO dolt_branch_control VALUES ('dolt','s1','ab','%','admin');",
                "INSERT INTO dolt_branch_control VALUES ('dolt','s2','ab','%','admin');",
                "INSERT INTO dolt_branch_control VALUES ('%','%','%','%','write');",
                "DELETE FROM dolt_branch_control;",
                "INSERT INTO dolt_branch_control VALUES ('dolt','s1','ab','%','admin');",
                "INSERT INTO dolt_branch_control VALUES ('dolt','s2','ab','%','admin');",
                "INSERT INTO dolt_branch_control VALUES ('%','%','%','%','write');",
                "DELETE FROM dolt_branch_control;",
                "INSERT INTO dolt_branch_control VALUES ('dolt','s1','ab','%','admin');",
                "INSERT INTO dolt_branch_control VALUES ('dolt','s2','ab','%','admin');",
                "INSERT INTO dolt_branch_control VALUES ('%','%','%','%','write');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_branch_control ORDER BY 1,2,3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("database", TEXT), Column("branch", TEXT), Column("user", TEXT), Column("host", TEXT), Column("permissions", TEXT)],
                        rows: &[
                            &[T("%"), T("%"), T("%"), T("%"), T("write")],
                            &[T("dolt"), T("s1"), T("ab"), T("%"), T("admin")],
                            &[T("dolt"), T("s2"), T("ab"), T("%"), T("admin")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM dolt_branch_control;"#,
                    expected: Expected::Tag("DELETE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT COUNT(1) FROM dolt_branch_control;"#,
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
                    query: r#"DELETE FROM dolt_branch_control;"#,
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT COUNT(1) FROM dolt_branch_control;"#,
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
                    query: r#"DELETE FROM dolt_branch_control;"#,
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT COUNT(1) FROM dolt_branch_control;"#,
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
                    query: r#"DELETE FROM dolt_branch_control;"#,
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT COUNT(1) FROM dolt_branch_control;"#,
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
            name: "branch-control: Issue #8622 ttask",
            intent: "The longest namespace rule matching a branch name decides who may create it, so ttask creates task_feature under task%.",
            database: "t",
            set_up_script: &[
                "CREATE USER tadmin PASSWORD 't';",
                "CREATE USER ttask PASSWORD 't';",
                "GRANT ALL ON DATABASE t TO tadmin;",
                "GRANT ALL ON DATABASE t TO ttask;",
                "INSERT INTO dolt_branch_namespace_control (database, branch, \"user\", host) VALUES ('t', '%', '', '');",
                "INSERT INTO dolt_branch_namespace_control (database, branch, \"user\", host) VALUES ('t', 'task%', 'ttask', '%');",
                "INSERT INTO dolt_branch_namespace_control (database, branch, \"user\", host) VALUES ('t', '%', 'tadmin', '%');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('task_feature');"#,
                    expected: Expected::Ok,
                    username: "ttask",
                    password: "t",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: Issue #8622 tadmin",
            intent: "A shorter namespace rule naming tadmin does not let it create a branch a longer rule reserves for ttask.",
            database: "t",
            set_up_script: &[
                "CREATE USER tadmin PASSWORD 't';",
                "CREATE USER ttask PASSWORD 't';",
                "GRANT ALL ON DATABASE t TO tadmin;",
                "GRANT ALL ON DATABASE t TO ttask;",
                "INSERT INTO dolt_branch_namespace_control (database, branch, \"user\", host) VALUES ('t', '%', '', '');",
                "INSERT INTO dolt_branch_namespace_control (database, branch, \"user\", host) VALUES ('t', 'task%', 'ttask', '%');",
                "INSERT INTO dolt_branch_namespace_control (database, branch, \"user\", host) VALUES ('t', '%', 'tadmin', '%');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_branch('task_feature');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"`tadmin`@`localhost` cannot create a branch named `task_feature`"#, ..E }),
                    username: "tadmin",
                    password: "t",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branch-control: Issue #8623",
            intent: "An admin rule on task% lets ttask stage on task_feature, whether it checks the branch out or connects to it directly.",
            database: "t",
            set_up_script: &[
                "CREATE USER ttask PASSWORD 't';",
                "GRANT ALL ON DATABASE t TO ttask;",
                "GRANT ALL ON SCHEMA public TO ttask;",
                "SELECT dolt_commit('-Am', 'users');",
                "DELETE FROM dolt_branch_namespace_control WHERE true;",
                "DELETE FROM dolt_branch_control WHERE true;",
                "SELECT * FROM dolt_branch_control;",
                "INSERT INTO dolt_branch_control (database, branch, \"user\", host, permissions) VALUES ('t', 'task%', 'ttask', '%', 'admin');",
                "SELECT dolt_branch('task_feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('task_feature');"#,
                    expected: Expected::Ok,
                    username: "ttask",
                    password: "t",
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT current_database(), active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("t"), T("task_feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "ttask",
                    password: "t",
                    client: "first",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('task_feature');"#,
                    expected: Expected::Ok,
                    username: "ttask",
                    password: "t",
                    client: "second",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Ok,
                    username: "ttask",
                    password: "t",
                    client: "second",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT current_database(), active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("t"), T("task_feature")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "ttask",
                    password: "t",
                    client: "third",
                    database: "t/task_feature",
                    dolt: Some(Dolt::Differs("current_database() names the base database, as pg_database does, and active_branch() names the revision, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Ok,
                    username: "ttask",
                    password: "t",
                    client: "fourth",
                    database: "t/task_feature",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
