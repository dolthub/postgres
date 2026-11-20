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

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_roles() {
    run_scripts(&[
        ScriptTest {
            name: "roles",
            set_up_script: &[
                "CREATE TABLE t (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT oid, rolname, rolsuper, rolinherit, rolcreaterole, rolcreatedb, rolcanlogin, rolreplication, rolconnlimit, rolpassword, rolvaliduntil, rolbypassrls, rolconfig FROM pg_roles ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID), Column("rolname", NAME), Column("rolsuper", BOOL), Column("rolinherit", BOOL), Column("rolcreaterole", BOOL), Column("rolcreatedb", BOOL), Column("rolcanlogin", BOOL), Column("rolreplication", BOOL), Column("rolconnlimit", INT4), Column("rolpassword", TEXT), Column("rolvaliduntil", TIMESTAMPTZ), Column("rolbypassrls", BOOL), Column("rolconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("10"), T("postgres"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("-1"), T("********"), Null, T("t"), Null],
                            &[T("3373"), T("pg_monitor"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("3374"), T("pg_read_all_settings"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("3375"), T("pg_read_all_stats"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("3377"), T("pg_stat_scan_tables"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("4200"), T("pg_signal_backend"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("4544"), T("pg_checkpoint"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("4550"), T("pg_use_reserved_connections"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("4569"), T("pg_read_server_files"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("4570"), T("pg_write_server_files"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("4571"), T("pg_execute_server_program"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("6171"), T("pg_database_owner"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("6181"), T("pg_read_all_data"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("6182"), T("pg_write_all_data"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("6304"), T("pg_create_subscription"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("6337"), T("pg_maintain"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                            &[T("6392"), T("pg_signal_autovacuum_worker"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("-1"), T("********"), Null, T("f"), Null],
                        ],
                        tag: "SELECT 17",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oid, rolname, rolpassword IS NULL AS nopassword FROM pg_authid ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID), Column("rolname", NAME), Column("nopassword", BOOL)],
                        rows: &[
                            &[T("10"), T("postgres"), T("t")],
                            &[T("3373"), T("pg_monitor"), T("t")],
                            &[T("3374"), T("pg_read_all_settings"), T("t")],
                            &[T("3375"), T("pg_read_all_stats"), T("t")],
                            &[T("3377"), T("pg_stat_scan_tables"), T("t")],
                            &[T("4200"), T("pg_signal_backend"), T("t")],
                            &[T("4544"), T("pg_checkpoint"), T("t")],
                            &[T("4550"), T("pg_use_reserved_connections"), T("t")],
                            &[T("4569"), T("pg_read_server_files"), T("t")],
                            &[T("4570"), T("pg_write_server_files"), T("t")],
                            &[T("4571"), T("pg_execute_server_program"), T("t")],
                            &[T("6171"), T("pg_database_owner"), T("t")],
                            &[T("6181"), T("pg_read_all_data"), T("t")],
                            &[T("6182"), T("pg_write_all_data"), T("t")],
                            &[T("6304"), T("pg_create_subscription"), T("t")],
                            &[T("6337"), T("pg_maintain"), T("t")],
                            &[T("6392"), T("pg_signal_autovacuum_worker"), T("t")],
                        ],
                        tag: "SELECT 17",
                    },
                    skip: Some("the Postgres template was made without a password for its bootstrap superuser, while Doltgrust's initdb stores config.yaml's, as initdb --pwfile does"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oid, roleid::regrole, member::regrole, grantor::regrole, admin_option, inherit_option, set_option FROM pg_auth_members ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID), Column("roleid", REGROLE), Column("member", REGROLE), Column("grantor", REGROLE), Column("admin_option", BOOL), Column("inherit_option", BOOL), Column("set_option", BOOL)],
                        rows: &[
                            &[T("10308"), T("pg_read_all_settings"), T("pg_monitor"), T("postgres"), T("f"), T("t"), T("t")],
                            &[T("10309"), T("pg_read_all_stats"), T("pg_monitor"), T("postgres"), T("f"), T("t"), T("t")],
                            &[T("10310"), T("pg_stat_scan_tables"), T("pg_monitor"), T("postgres"), T("f"), T("t"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname, r.rolsuper, r.rolinherit, r.rolcreaterole, r.rolcreatedb, r.rolcanlogin, r.rolconnlimit, r.rolvaliduntil, r.rolreplication, r.rolbypassrls FROM pg_catalog.pg_roles r WHERE r.rolname !~ '^pg_' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolsuper", BOOL), Column("rolinherit", BOOL), Column("rolcreaterole", BOOL), Column("rolcreatedb", BOOL), Column("rolcanlogin", BOOL), Column("rolconnlimit", INT4), Column("rolvaliduntil", TIMESTAMPTZ), Column("rolreplication", BOOL), Column("rolbypassrls", BOOL)],
                        rows: &[
                            &[T("postgres"), T("t"), T("t"), T("t"), T("t"), T("t"), T("-1"), Null, T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_userbyid(10), pg_get_userbyid(6171), pg_get_userbyid(12345);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_userbyid", NAME), Column("pg_get_userbyid", NAME), Column("pg_get_userbyid", NAME)],
                        rows: &[
                            &[T("postgres"), T("pg_database_owner"), T("unknown (OID=12345)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, pg_catalog.pg_get_userbyid(c.relowner) AS owner FROM pg_catalog.pg_class c WHERE c.relname = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("owner", NAME)],
                        rows: &[
                            &[T("t"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n.nspname, pg_catalog.pg_get_userbyid(n.nspowner) AS owner FROM pg_catalog.pg_namespace n WHERE n.nspname IN ('public', 'pg_catalog') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME), Column("owner", NAME)],
                        rows: &[
                            &[T("pg_catalog"), T("postgres")],
                            &[T("public"), T("pg_database_owner")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'postgres'::regrole, '"pg_monitor"'::regrole, 'PG_MONITOR'::regrole::oid, '6171'::regrole, '-'::regrole::oid, 99999::regrole;"#,
                    expected: Expected::Rows {
                        columns: &[Column("regrole", REGROLE), Column("regrole", REGROLE), Column("oid", OID), Column("regrole", REGROLE), Column("oid", OID), Column("regrole", REGROLE)],
                        rows: &[
                            &[T("postgres"), T("pg_monitor"), T("3373"), T("pg_database_owner"), T("0"), T("99999")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'nosuch'::regrole;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuch" does not exist"#, position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a.b'::regrole;",
                    expected: Expected::Error(Diagnostic { code: "42602", message: "invalid name syntax", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_regrole('pg_maintain'), to_regrole('nosuch'), to_regrole('a.b');",
                    expected: Expected::Rows {
                        columns: &[Column("to_regrole", REGROLE), Column("to_regrole", REGROLE), Column("to_regrole", REGROLE)],
                        rows: &[
                            &[T("pg_maintain"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'postgres'::regrole = 10 AS bootstrap;",
                    expected: Expected::Rows {
                        columns: &[Column("bootstrap", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rs (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rs ENABLE ROW LEVEL SECURITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_security_active('rs'), row_security_active('rs'::regclass), row_security_active('public.rs');",
                    expected: Expected::Rows {
                        columns: &[Column("row_security_active", BOOL), Column("row_security_active", BOOL), Column("row_security_active", BOOL)],
                        rows: &[
                            &[T("f"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_security_active('nosuch');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_create_alter_and_drop_role() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE, ALTER, and DROP ROLE",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE r1;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r1;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"role "r1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE USER u1 PASSWORD 'secret' CREATEDB VALID UNTIL '2030-01-01 00:00:00+00' CONNECTION LIMIT 5;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE pg_bad;",
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"role name "pg_bad" is reserved"#, detail: r#"Role names starting with "pg_" are reserved."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r2 IN ROLE r1 ROLE u1 ADMIN r1;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: r#"role "r2" is a member of role "r1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r3 CONNECTION LIMIT -2;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid connection limit: -2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r3 SUPERUSER NOSUPERUSER;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "conflicting or redundant options", position: 26, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TimeZone = 'UTC';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname, rolsuper, rolinherit, rolcreaterole, rolcreatedb, rolcanlogin, rolreplication, rolbypassrls, rolconnlimit, rolvaliduntil, rolpassword IS NOT NULL AS haspw, left(rolpassword, 14) AS pwprefix FROM pg_authid WHERE rolname !~ '^pg_' AND rolname <> 'postgres' ORDER BY rolname;",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolsuper", BOOL), Column("rolinherit", BOOL), Column("rolcreaterole", BOOL), Column("rolcreatedb", BOOL), Column("rolcanlogin", BOOL), Column("rolreplication", BOOL), Column("rolbypassrls", BOOL), Column("rolconnlimit", INT4), Column("rolvaliduntil", TIMESTAMPTZ), Column("haspw", BOOL), Column("pwprefix", TEXT)],
                        rows: &[
                            &[T("r1"), T("f"), T("t"), T("f"), T("f"), T("f"), T("f"), T("f"), T("-1"), Null, T("f"), Null],
                            &[T("u1"), T("f"), T("t"), T("f"), T("t"), T("t"), T("f"), T("f"), T("5"), T("2030-01-01 00:00:00+00"), T("t"), T("SCRAM-SHA-256$")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname AS role, m.rolname AS member, g.rolname AS grantor, admin_option, inherit_option, set_option FROM pg_auth_members am JOIN pg_authid r ON r.oid = am.roleid JOIN pg_authid m ON m.oid = am.member JOIN pg_authid g ON g.oid = am.grantor WHERE r.rolname !~ '^pg_' ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("role", NAME), Column("member", NAME), Column("grantor", NAME), Column("admin_option", BOOL), Column("inherit_option", BOOL), Column("set_option", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r1 CREATEROLE LOGIN CONNECTION LIMIT 3;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE pg_monitor LOGIN;",
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"role name "pg_monitor" is reserved"#, detail: "Cannot alter reserved roles.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE nosuch LOGIN;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE postgres NOSUPERUSER;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "permission denied to alter role", detail: "The bootstrap superuser must have the SUPERUSER attribute.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE u1 PASSWORD NULL;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE u1 PASSWORD '';",
                    expected: Expected::Tag("ALTER ROLE"),
                    notices: &[Diagnostic { code: "00000", message: "empty string is not a valid password, clearing password", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET password_encryption = 'md5';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE u1 PASSWORD 'secret';",
                    expected: Expected::Tag("ALTER ROLE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01P01", message: "setting an MD5-encrypted password", detail: "MD5 password support is deprecated and will be removed in a future release of PostgreSQL.", hint: "Refer to the PostgreSQL documentation for details about migrating to another password type.", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname, rolpassword FROM pg_authid WHERE rolname = 'u1';",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolpassword", TEXT)],
                        rows: &[
                            &[T("u1"), T("md535a53ec164a78bb7375618b79dd88cf5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE u1 RENAME TO u2;",
                    expected: Expected::Tag("ALTER ROLE"),
                    notices: &[Diagnostic { code: "00000", message: "MD5 password cleared because of role rename", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname, rolpassword FROM pg_authid WHERE rolname = 'u2';",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolpassword", TEXT)],
                        rows: &[
                            &[T("u2"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE u2 RENAME TO r1;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"role "r1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE u2 RENAME TO pg_x;",
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"role name "pg_x" is reserved"#, detail: r#"Role names starting with "pg_" are reserved."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER GROUP r1 ADD USER u2;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER GROUP r1 DROP USER u2;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER GROUP r1 DROP USER u2;",
                    expected: Expected::Tag("ALTER ROLE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"role "u2" has not been granted membership in role "r1" by role "postgres""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER GROUP r2 ADD USER r1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "r2" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE IF EXISTS nosuch;",
                    expected: Expected::Tag("DROP ROLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"role "nosuch" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE pg_monitor;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop role pg_monitor because it is required by the database system", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE postgres;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: "current user cannot be dropped", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE CURRENT_USER;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot use special role specifier in DROP ROLE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE t1 IN ROLE r1 ROLE u2 PASSWORD 'x';",
                    expected: Expected::Tag("CREATE ROLE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01P01", message: "setting an MD5-encrypted password", detail: "MD5 password support is deprecated and will be removed in a future release of PostgreSQL.", hint: "Refer to the PostgreSQL documentation for details about migrating to another password type.", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_authid WHERE rolname = 't1';",
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
                    query: "DROP ROLE r1, r2, u2;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "r2" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname FROM pg_authid WHERE rolname !~ '^pg_' AND rolname <> 'postgres' ORDER BY rolname;",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME)],
                        rows: &[
                            &[T("r1")],
                            &[T("u2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_auth_members WHERE member >= 16384 OR roleid >= 16384;",
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
    ]);
}

#[test]
fn test_role_commands_of_createrole_roles() {
    run_scripts(&[
        ScriptTest {
            name: "role commands of CREATEROLE roles",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE creator CREATEROLE LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE plain LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("creator")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE c1;",
                    expected: Expected::Tag("CREATE ROLE"),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE c2 SUPERUSER;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create role", detail: "Only roles with the SUPERUSER attribute may create roles with the SUPERUSER attribute.", ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE c2 CREATEDB;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create role", detail: "Only roles with the CREATEDB attribute may create roles with the CREATEDB attribute.", ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE c2 REPLICATION;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create role", detail: "Only roles with the REPLICATION attribute may create roles with the REPLICATION attribute.", ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE c2 BYPASSRLS;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create role", detail: "Only roles with the BYPASSRLS attribute may create roles with the BYPASSRLS attribute.", ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE c2 IN ROLE other;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to grant role "other""#, detail: r#"Only roles with the ADMIN option on role "other" may grant this role."#, ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET createrole_self_grant = 'set, inherit';",
                    expected: Expected::Tag("SET"),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE c3 ROLE plain ADMIN other;",
                    expected: Expected::Tag("CREATE ROLE"),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE c1 LOGIN;",
                    expected: Expected::Tag("ALTER ROLE"),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE other LOGIN;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter role", detail: r#"Only roles with the CREATEROLE attribute and the ADMIN option on role "other" may alter this role."#, ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE other PASSWORD 'zzz';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter role", detail: "To change another role's password, the current user must have the CREATEROLE attribute and the ADMIN option on the role.", ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE creator PASSWORD 'password';",
                    expected: Expected::Tag("ALTER ROLE"),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE c1 CREATEDB;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter role", detail: "Only roles with the CREATEDB attribute may change the CREATEDB attribute.", ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE c1 SUPERUSER;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter role", detail: "Only roles with the SUPERUSER attribute may change the SUPERUSER attribute.", ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE c1 RENAME TO c1b;",
                    expected: Expected::Tag("ALTER ROLE"),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE other RENAME TO other2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to rename role", detail: r#"Only roles with the CREATEROLE attribute and the ADMIN option on role "other" may rename this role."#, ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE creator RENAME TO creator2;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "session user cannot be renamed", ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE other;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to drop role", detail: r#"Only roles with the CREATEROLE attribute and the ADMIN option on role "other" may drop this role."#, ..E }),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE c1b;",
                    expected: Expected::Tag("DROP ROLE"),
                    username: "creator",
                    password: "password",
                    client: "creator",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE p1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create role", detail: "Only roles with the CREATEROLE attribute may create roles.", ..E }),
                    username: "plain",
                    password: "password",
                    client: "plain",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE c3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to drop role", detail: "Only roles with the CREATEROLE attribute and the ADMIN option on the target roles may drop roles.", ..E }),
                    username: "plain",
                    password: "password",
                    client: "plain",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE plain PASSWORD 'password';",
                    expected: Expected::Tag("ALTER ROLE"),
                    username: "plain",
                    password: "password",
                    client: "plain",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE plain VALID UNTIL 'infinity';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter role", detail: r#"Only roles with the CREATEROLE attribute and the ADMIN option on role "plain" may alter this role."#, ..E }),
                    username: "plain",
                    password: "password",
                    client: "plain",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname AS role, m.rolname AS member, g.rolname AS grantor, admin_option, inherit_option, set_option FROM pg_auth_members am JOIN pg_authid r ON r.oid = am.roleid JOIN pg_authid m ON m.oid = am.member JOIN pg_authid g ON g.oid = am.grantor WHERE r.rolname !~ '^pg_' ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("role", NAME), Column("member", NAME), Column("grantor", NAME), Column("admin_option", BOOL), Column("inherit_option", BOOL), Column("set_option", BOOL)],
                        rows: &[
                            &[T("c3"), T("creator"), T("creator"), T("f"), T("t"), T("t")],
                            &[T("c3"), T("creator"), T("postgres"), T("t"), T("f"), T("f")],
                            &[T("c3"), T("other"), T("creator"), T("t"), T("t"), T("t")],
                            &[T("c3"), T("plain"), T("creator"), T("f"), T("t"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE creator;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "creator" cannot be dropped because some objects depend on it"#, detail: "privileges for membership of role plain in role c3\nprivileges for membership of role other in role c3", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname FROM pg_authid WHERE rolname !~ '^pg_' AND rolname <> 'postgres' ORDER BY rolname;",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME)],
                        rows: &[
                            &[T("c3")],
                            &[T("creator")],
                            &[T("other")],
                            &[T("plain")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA owned AUTHORIZATION plain;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE plain;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "plain" cannot be dropped because some objects depend on it"#, detail: "owner of schema owned", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA owned;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE plain;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE a1;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE a2;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE a3;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER GROUP a1 ADD USER a2;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE a1, a2;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE a3;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname FROM pg_authid WHERE rolname !~ '^pg_' AND rolname <> 'postgres' ORDER BY rolname;",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME)],
                        rows: &[
                            &[T("c3")],
                            &[T("creator")],
                            &[T("other")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE g1;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON t TO g1;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE relname = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,g1=r/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA s2 AUTHORIZATION g1;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspname, nspowner::regrole FROM pg_namespace WHERE nspname = 's2';",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME), Column("nspowner", REGROLE)],
                        rows: &[
                            &[T("s2"), T("g1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE g1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "g1" cannot be dropped because some objects depend on it"#, detail: "privileges for table t\nowner of schema s2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA s2;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SELECT ON t FROM g1;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE relname = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres}")],
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
fn test_create_role_if_not_exists() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE ROLE and CREATE USER IF NOT EXISTS",
            set_up_script: &[],
            assertions: &[
                // Doltgrust-specific: Postgres has no IF NOT EXISTS here, so Doltgrust's own output is expected.
                ScriptTestAssertion {
                    query: "CREATE ROLE IF NOT EXISTS ine;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres has no IF NOT EXISTS here, so Doltgrust's own output is expected.
                ScriptTestAssertion {
                    query: "CREATE ROLE IF NOT EXISTS ine;",
                    expected: Expected::Tag("CREATE ROLE"),
                    notices: &[Diagnostic { code: "42710", message: r#"role "ine" already exists, skipping"#, ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres has no IF NOT EXISTS here, so Doltgrust's own output is expected.
                ScriptTestAssertion {
                    query: "CREATE USER IF NOT EXISTS ine PASSWORD 'x';",
                    expected: Expected::Tag("CREATE ROLE"),
                    notices: &[Diagnostic { code: "42710", message: r#"role "ine" already exists, skipping"#, ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres has no IF NOT EXISTS here, so Doltgrust's own output is expected.
                ScriptTestAssertion {
                    query: "CREATE USER IF NOT EXISTS ine2;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname, rolcanlogin, rolpassword FROM pg_authid WHERE rolname LIKE 'ine%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("rolcanlogin", BOOL), Column("rolpassword", TEXT)],
                        rows: &[
                            &[T("ine"), T("f"), Null],
                            &[T("ine2"), T("t"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_rolled_back_role_commands() {
    run_scripts(&[
        ScriptTest {
            name: "a rolled back CREATE ROLE leaves no role, membership, or password",
            set_up_script: &[
                "CREATE ROLE rb_group;",
                "CREATE ROLE rb_member;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE rb IN ROLE rb_group ROLE rb_member PASSWORD 'secret';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_auth_members WHERE roleid = 'rb'::regrole OR member = 'rb'::regrole;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_authid WHERE rolname = 'rb';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_auth_members m JOIN pg_authid r ON r.oid = m.roleid JOIN pg_authid u ON u.oid = m.member WHERE r.rolname LIKE 'rb%' OR u.rolname LIKE 'rb%';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE rb;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolpassword FROM pg_authid WHERE rolname = 'rb';",
                    expected: Expected::Rows { columns: &[Column("rolpassword", TEXT)], rows: &[&[Null]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_concurrent_role_commands() {
    run_scripts(&[
        ScriptTest {
            name: "concurrent role commands",
            set_up_script: &[
                "CREATE TABLE t (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ca IN ROLE pg_monitor;",
                    expected: Expected::Tag("CREATE ROLE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE cb;",
                    expected: Expected::Tag("CREATE ROLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ca;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "pg_authid_rolname_index""#, detail: "Key (rolname)=(ca) already exists.", ..E }),
                    client: "c",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname FROM pg_roles WHERE rolname IN ('ca', 'cb') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME)],
                        rows: &[
                            &[T("ca")],
                            &[T("cb")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE relname = 't';",
                    expected: Expected::Rows { columns: &[Column("relacl", ACLITEM_ARRAY)], rows: &[&[Null]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON t TO ca;",
                    expected: Expected::Tag("GRANT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relacl FROM pg_class WHERE relname = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("relacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("{postgres=arwdDxtm/postgres,ca=r/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "d",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows { columns: &[Column("?column?", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "d",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d7157;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_database WHERE datname = 'd7157';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    client: "d",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_database WHERE datname = 'd7157';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "e",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "d",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_saslprep_passwords() {
    run_scripts(&[
        ScriptTest {
            name: "passwords are prepared with SASLprep",
            set_up_script: &[
                "CREATE ROLE sp LOGIN PASSWORD E'\\uFB01\\u00AD';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows { columns: &[Column("current_user", NAME)], rows: &[&[T("sp")]], tag: "SELECT 1" },
                    username: "sp",
                    password: "fi",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_grant_and_revoke_role() {
    run_scripts(&[
        ScriptTest {
            name: "GRANT and REVOKE of role memberships",
            set_up_script: &[
                "CREATE ROLE g1;",
                "CREATE ROLE g2;",
                "CREATE ROLE m1 LOGIN PASSWORD 'password';",
                "CREATE ROLE m2 NOINHERIT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "GRANT g1 TO m1;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO m1;",
                    expected: Expected::Tag("GRANT ROLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"role "m1" has already been granted membership in role "g1" by role "postgres""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO m2 WITH ADMIN OPTION, INHERIT TRUE, SET FALSE;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO m2 WITH ADMIN TRUE;",
                    expected: Expected::Tag("GRANT ROLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"role "m2" has already been granted membership in role "g1" by role "postgres""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO m2 WITH BOGUS TRUE;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized role option "bogus""#, position: 21, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 (a) TO m1;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "column names cannot be included in GRANT/REVOKE ROLE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT m1 TO g1;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: r#"role "m1" is a member of role "g1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO g1;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: r#"role "g1" is a member of role "g1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO PUBLIC;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "public" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT nosuch TO m1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT pg_database_owner TO m1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"role "pg_database_owner" cannot have explicit members"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g2 TO pg_database_owner;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"role "pg_database_owner" cannot be a member of any role"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1, g2 TO m1, m2 GRANTED BY m2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to grant privileges as role "m2""#, detail: r#"The grantor must have the ADMIN option on role "g2"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g2 TO m1 GRANTED BY CURRENT_USER;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname AS role, m.rolname AS member, g.rolname AS grantor, admin_option, inherit_option, set_option FROM pg_auth_members am JOIN pg_authid r ON r.oid = am.roleid JOIN pg_authid m ON m.oid = am.member JOIN pg_authid g ON g.oid = am.grantor WHERE r.rolname !~ '^pg_' ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("role", NAME), Column("member", NAME), Column("grantor", NAME), Column("admin_option", BOOL), Column("inherit_option", BOOL), Column("set_option", BOOL)],
                        rows: &[
                            &[T("g1"), T("m1"), T("postgres"), T("f"), T("t"), T("t")],
                            &[T("g1"), T("m2"), T("postgres"), T("t"), T("t"), T("f")],
                            &[T("g2"), T("m1"), T("postgres"), T("f"), T("t"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO m2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to grant role "g1""#, detail: r#"Only roles with the ADMIN option on role "g1" may grant this role."#, ..E }),
                    username: "m1",
                    password: "password",
                    client: "m1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g2 TO m2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to grant role "g2""#, detail: r#"Only roles with the ADMIN option on role "g2" may grant this role."#, ..E }),
                    username: "m1",
                    password: "password",
                    client: "m1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO m1 WITH ADMIN OPTION;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO g2;",
                    expected: Expected::Tag("GRANT ROLE"),
                    username: "m1",
                    password: "password",
                    client: "m1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT g1 TO m2 GRANTED BY postgres;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to grant privileges as role "postgres""#, detail: r#"Only roles with privileges of role "postgres" may grant privileges as this role."#, ..E }),
                    username: "m1",
                    password: "password",
                    client: "m1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE g1 FROM m1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "dependent privileges exist", hint: "Use CASCADE to revoke them too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE g1 FROM m1 CASCADE;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE g1 FROM m1;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"role "m1" has not been granted membership in role "g1" by role "postgres""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ADMIN OPTION FOR g1 FROM m2;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE INHERIT OPTION FOR g1 FROM m2;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SET OPTION FOR g1 FROM m2;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE BOGUS OPTION FOR g1 FROM m2;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized role option "bogus""#, position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE g2 FROM m1 GRANTED BY postgres;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE g2 FROM m1;",
                    expected: Expected::Tag("REVOKE ROLE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"role "m1" has not been granted membership in role "g2" by role "postgres""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.rolname AS role, m.rolname AS member, g.rolname AS grantor, admin_option, inherit_option, set_option FROM pg_auth_members am JOIN pg_authid r ON r.oid = am.roleid JOIN pg_authid m ON m.oid = am.member JOIN pg_authid g ON g.oid = am.grantor WHERE r.rolname !~ '^pg_' ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("role", NAME), Column("member", NAME), Column("grantor", NAME), Column("admin_option", BOOL), Column("inherit_option", BOOL), Column("set_option", BOOL)],
                        rows: &[
                            &[T("g1"), T("m2"), T("postgres"), T("f"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE m2;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE g1, g2, m1, m2;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "m2" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_set_role_and_security_definer() {
    run_scripts(&[
        ScriptTest {
            name: "SET ROLE, SET SESSION AUTHORIZATION, and SECURITY DEFINER",
            set_up_script: &[
                "CREATE ROLE sr1 LOGIN PASSWORD 'password';",
                "CREATE ROLE sr2;",
                "CREATE ROLE sr3;",
                "CREATE ROLE srsuper SUPERUSER;",
                "GRANT sr2 TO sr1;",
                "GRANT sr3 TO sr1 WITH SET FALSE;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user, current_role;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME), Column("current_role", NAME)],
                        rows: &[
                            &[T("postgres"), T("postgres"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW is_superuser;",
                    expected: Expected::Rows {
                        columns: &[Column("is_superuser", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW role;",
                    expected: Expected::Rows {
                        columns: &[Column("role", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sr2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user, current_role;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME), Column("current_role", NAME)],
                        rows: &[
                            &[T("sr2"), T("postgres"), T("sr2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW is_superuser;",
                    expected: Expected::Rows {
                        columns: &[Column("is_superuser", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW role;",
                    expected: Expected::Rows {
                        columns: &[Column("role", TEXT)],
                        rows: &[
                            &[T("sr2")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE srsuper;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW is_superuser;",
                    expected: Expected::Rows {
                        columns: &[Column("is_superuser", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE NONE;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("postgres"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW role;",
                    expected: Expected::Rows {
                        columns: &[Column("role", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE nosuch;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"role "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION sr1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("sr1"), T("sr1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW is_superuser;",
                    expected: Expected::Rows {
                        columns: &[Column("is_superuser", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sr2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("sr2"), T("sr1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sr3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set role "sr3""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE srsuper;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set role "srsuper""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION srsuper;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("srsuper"), T("srsuper")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("postgres"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW role;",
                    expected: Expected::Rows {
                        columns: &[Column("role", TEXT)],
                        rows: &[
                            &[T("none")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("postgres"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL ROLE sr2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("sr2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sr2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sr2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sr2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to drop role", detail: "Only roles with the CREATEROLE attribute and the ADMIN option on the target roles may drop roles.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("sr1"), T("sr1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW is_superuser;",
                    expected: Expected::Rows {
                        columns: &[Column("is_superuser", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SHOW",
                    },
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sr2;",
                    expected: Expected::Tag("SET"),
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("sr2"), T("sr1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sr3;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set role "sr3""#, ..E }),
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION postgres;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set session authorization "postgres""#, ..E }),
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION sr1;",
                    expected: Expected::Tag("SET"),
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f_inv() RETURNS text LANGUAGE sql AS 'SELECT current_user::text';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f_def() RETURNS text LANGUAGE sql SECURITY DEFINER AS 'SELECT current_user::text || '' '' || session_user::text';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f_setrole() RETURNS text LANGUAGE plpgsql SECURITY DEFINER AS $$ BEGIN SET ROLE sr2; RETURN current_user; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION f_setrole_inv() RETURNS text LANGUAGE plpgsql AS $$ BEGIN SET ROLE sr2; RETURN current_user; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT EXECUTE ON FUNCTION f_def() TO sr1;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_def();",
                    expected: Expected::Rows {
                        columns: &[Column("f_def", TEXT)],
                        rows: &[
                            &[T("postgres sr1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("sr1")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_setrole();",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"cannot set parameter "role" within security-definer function"#, context: Some("SQL statement \"SET ROLE sr2\"\nPL/pgSQL function f_setrole() line 1 at SQL statement"), ..E }),
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_setrole_inv();",
                    expected: Expected::Rows {
                        columns: &[Column("f_setrole_inv", TEXT)],
                        rows: &[
                            &[T("sr2")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("sr2")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    username: "sr1",
                    password: "password",
                    client: "sr1",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_def();",
                    expected: Expected::Rows {
                        columns: &[Column("f_def", TEXT)],
                        rows: &[
                            &[T("postgres postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE srsuper;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET SESSION AUTHORIZATION sr1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE postgres;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to drop role", detail: "Only roles with the CREATEROLE attribute and the ADMIN option on the target roles may drop roles.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET SESSION AUTHORIZATION;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sr1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sr1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to drop role", detail: "Only roles with the CREATEROLE attribute and the ADMIN option on the target roles may drop roles.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE sr1 RENAME TO sr1x;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "current user cannot be renamed", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION f_def(), f_setrole(), f_setrole_inv();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE srsuper2 SUPERUSER;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE srsuper2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE srsuper2;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: "current user cannot be dropped", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE postgres;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: "session user cannot be dropped", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE postgres RENAME TO pgx;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "session user cannot be renamed", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, session_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("session_user", NAME)],
                        rows: &[
                            &[T("srsuper2"), T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW is_superuser;",
                    expected: Expected::Rows {
                        columns: &[Column("is_superuser", TEXT)],
                        rows: &[
                            &[T("on")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_maintenance_as_table_owner() {
    run_scripts(&[
        ScriptTest {
            name: "maintenance runs as the table's owner in a security-restricted operation",
            set_up_script: &[
                "CREATE TABLE ti (a int);",
                "INSERT INTO ti VALUES (1), (2);",
                "CREATE FUNCTION v_temp() RETURNS int LANGUAGE plpgsql AS $$ BEGIN CREATE TEMP TABLE tt (x int); RETURN 1; END $$;",
                "CREATE FUNCTION v_role() RETURNS int LANGUAGE plpgsql AS $$ BEGIN SET ROLE postgres; RETURN 1; END $$;",
                "CREATE FUNCTION v_prep() RETURNS int LANGUAGE plpgsql AS $$ BEGIN EXECUTE 'PREPARE p AS SELECT 1'; RETURN 1; END $$;",
                "CREATE FUNCTION v_listen() RETURNS int LANGUAGE plpgsql AS $$ BEGIN LISTEN chan; RETURN 1; END $$;",
                "CREATE FUNCTION v_path() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'path % as %', current_setting('search_path'), current_user; END $$;",
                "CREATE FUNCTION i_temp(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN public.v_temp(); END $$;",
                "CREATE FUNCTION i_role(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN public.v_role(); END $$;",
                "CREATE FUNCTION i_prep(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN public.v_prep(); END $$;",
                "CREATE FUNCTION i_listen(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN public.v_listen(); END $$;",
                "CREATE FUNCTION i_path(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN public.v_path(); END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX ON ti (i_temp(a));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot create temporary table within security-restricted operation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON ti (i_role(a));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"cannot set parameter "role" within security-restricted operation"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON ti (i_prep(a));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot execute PREPARE within security-restricted operation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON ti (i_listen(a));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot execute LISTEN within security-restricted operation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON ti (i_path(a));",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "path pg_catalog, pg_temp as postgres", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, current_setting('search_path');",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("postgres"), T(r#""$user", public"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE tt2 (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ts VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION e_path(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RAISE EXCEPTION 'stats path % as %', current_setting('search_path'), current_user; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS sts1 ON (e_path(a)) FROM ts;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ts;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "stats path pg_catalog, pg_temp as postgres", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS sts1;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS sts2 ON (i_temp(a)) FROM ts;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ts;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "cannot create temporary table within security-restricted operation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS sts2;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS sts3 ON (i_role(a)) FROM ts;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ts;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"cannot set parameter "role" within security-restricted operation"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS sts3;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ts;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user, current_setting('search_path');",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("postgres"), T(r#""$user", public"#)],
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
fn test_roles_and_databases_have_oids_of_their_own() {
    run_scripts(&[
        ScriptTest {
            name: "a new role's and a new database's OIDs differ from each other and from every relation's and type's",
            set_up_script: &[
                "CREATE ROLE shared_oid_role;",
                "CREATE DATABASE shared_oid_db;",
                "CREATE TABLE shared_oid_t (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'shared_oid_role'::REGROLE::OID <> (SELECT oid FROM pg_database WHERE datname = 'shared_oid_db') AS distinct_oids, NOT EXISTS (SELECT 1 FROM pg_class WHERE oid IN ('shared_oid_role'::REGROLE::OID, (SELECT oid FROM pg_database WHERE datname = 'shared_oid_db'))) AS no_relation, NOT EXISTS (SELECT 1 FROM pg_type WHERE oid IN ('shared_oid_role'::REGROLE::OID, (SELECT oid FROM pg_database WHERE datname = 'shared_oid_db'))) AS no_type;",
                    expected: Expected::Rows {
                        columns: &[Column("distinct_oids", BOOL), Column("no_relation", BOOL), Column("no_type", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
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
fn test_passwords_of_the_empty_string_are_cleared() {
    run_scripts(&[
        ScriptTest {
            name: "a password given as an MD5 or SCRAM hash of the empty string is cleared like an empty one",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_passwd_empty PASSWORD '';",
                    expected: Expected::Tag("CREATE ROLE"),
                    notices: &[Diagnostic { code: "00000", message: "empty string is not a valid password, clearing password", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE regress_passwd_empty PASSWORD 'md585939a5ce845f1a1b620742e3c659e0a';",
                    expected: Expected::Tag("ALTER ROLE"),
                    notices: &[Diagnostic { code: "00000", message: "empty string is not a valid password, clearing password", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE regress_passwd_empty PASSWORD 'SCRAM-SHA-256$4096:hpFyHTUsSWcR7O9P$LgZFIt6Oqdo27ZFKbZ2nV+vtnYM995pDh9ca6WSi120=:qVV5NeluNfUPkwm7Vqat25RjSPLkGeoZBQs6wVv+um4=';",
                    expected: Expected::Tag("ALTER ROLE"),
                    notices: &[Diagnostic { code: "00000", message: "empty string is not a valid password, clearing password", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolpassword FROM pg_authid WHERE rolname = 'regress_passwd_empty';",
                    expected: Expected::Rows {
                        columns: &[Column("rolpassword", TEXT)],
                        rows: &[
                            &[Null],
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
fn test_roles_dropped_while_objects_come_to_depend_on_them() {
    run_scripts(&[
        ScriptTest {
            name: "an object a session makes for a role that another session drops meanwhile fails",
            set_up_script: &[
                "CREATE ROLE sd_owner;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_owner;",
                    expected: Expected::Tag("DROP ROLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sd_s AUTHORIZATION sd_owner;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "was concurrently dropped", message_contains: true, ..E }),
                    client: "B", expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a grant to a role that another session drops meanwhile fails",
            set_up_script: &[
                "CREATE ROLE sd_grantee;",
                "CREATE TABLE sd_t (x INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_grantee;",
                    expected: Expected::Tag("DROP ROLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON sd_t TO sd_grantee;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "was concurrently dropped", message_contains: true, ..E }),
                    client: "B", expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a new owner that another session drops meanwhile fails the change of owner",
            set_up_script: &[
                "CREATE ROLE sd_new;",
                "CREATE TABLE sd_u (x INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_new;",
                    expected: Expected::Tag("DROP ROLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sd_u OWNER TO sd_new;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "was concurrently dropped", message_contains: true, ..E }),
                    client: "B", expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping a role waits for a transaction that made an object for it and goes ahead once it rolls back",
            set_up_script: &[
                "CREATE ROLE sd_wait;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sd_w AUTHORIZATION sd_wait;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_wait;",
                    expected: Expected::Tag("DROP ROLE"),
                    client: "B", expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "two sessions make objects for one role without waiting for each other",
            set_up_script: &[
                "CREATE ROLE sd_both;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sd_a AUTHORIZATION sd_both;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA sd_b AUTHORIZATION sd_both;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA sd_a, sd_b;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_both;",
                    expected: Expected::Tag("DROP ROLE"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a grant to a role another session dropped since the transaction began fails",
            set_up_script: &["CREATE ROLE sd_late;", "CREATE TABLE sd_v (x INT);"],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), client: "A", ..A },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sd_v;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion { query: "DROP ROLE sd_late;", expected: Expected::Tag("DROP ROLE"), client: "B", ..A },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON sd_v TO sd_late;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "sd_late" does not exist"#, ..E }),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), client: "A", ..A },
            ],
            ..S
        },
    ]);
}
