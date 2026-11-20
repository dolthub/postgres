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
fn test_parameter_and_database_privileges() {
    run_scripts(&[
        ScriptTest {
            name: "GRANT ON PARAMETER, GRANT ON DATABASE, and CONNECT",
            set_up_script: &[
                "CREATE ROLE r6p LOGIN PASSWORD 'password';",
                "CREATE ROLE r6q LOGIN PASSWORD 'password';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "GRANT SET ON PARAMETER log_statement TO r6p;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO r6p;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALTER SYSTEM ON PARAMETER work_mem TO r6p WITH GRANT OPTION;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SET, ALTER SYSTEM ON PARAMETER "TimeZone", my.custom_thing TO r6q;"#,
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SET ON PARAMETER nosuch TO r6p;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized configuration parameter "nosuch""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SET ON PARAMETER "bad name" TO r6p;"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized configuration parameter "bad name""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON PARAMETER work_mem TO r6p;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type SELECT for parameter", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON PARAMETER DateStyle TO r6q;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parname, paracl FROM pg_parameter_acl ORDER BY parname;",
                    expected: Expected::Rows {
                        columns: &[Column("parname", TEXT), Column("paracl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("datestyle"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                            &[T("log_statement"), T("{postgres=sA/postgres,r6p=s/postgres}")],
                            &[T("my.custom_thing"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                            &[T("timezone"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                            &[T("work_mem"), T("{postgres=sA/postgres,r6p=A*/postgres}")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_parameter_privilege('r6p', 'log_statement', 'SET'), has_parameter_privilege('r6p', 'LOG_STATEMENT', 'ALTER SYSTEM'), has_parameter_privilege('r6q', 'timezone', 'SET, ALTER SYSTEM'), has_parameter_privilege('r6p', 'work_mem', 'ALTER SYSTEM WITH GRANT OPTION');",
                    expected: Expected::Rows {
                        columns: &[Column("has_parameter_privilege", BOOL), Column("has_parameter_privilege", BOOL), Column("has_parameter_privilege", BOOL), Column("has_parameter_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_parameter_privilege('r6p', 'nosuch', 'SET');",
                    expected: Expected::Rows {
                        columns: &[Column("has_parameter_privilege", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_parameter_privilege('r6p', 'log_statement', 'SELECT');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"unrecognized privilege type: "SELECT""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET log_statement = 'all';",
                    expected: Expected::Tag("SET"),
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW log_statement;",
                    expected: Expected::Rows {
                        columns: &[Column("log_statement", TEXT)],
                        rows: &[
                            &[T("all")],
                        ],
                        tag: "SHOW",
                    },
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET log_min_duration_statement = 10;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set parameter "log_min_duration_statement""#, ..E }),
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALTER SYSTEM ON PARAMETER work_mem TO r6q;",
                    expected: Expected::Tag("GRANT"),
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SET ON PARAMETER work_mem TO r6q;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01007", message: r#"no privileges were granted for "work_mem""#, ..N }],
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SET ON PARAMETER log_statement TO r6q;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01007", message: r#"no privileges were granted for "log_statement""#, ..N }],
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION r6f() RETURNS int LANGUAGE sql SET log_statement = 'ddl' AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION r6g() RETURNS int LANGUAGE sql SET log_min_duration_statement = 10 AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set parameter "log_min_duration_statement""#, ..E }),
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6p SET log_statement = 'mod';",
                    expected: Expected::Tag("ALTER ROLE"),
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6p SET log_min_duration_statement = 10;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set parameter "log_min_duration_statement""#, ..E }),
                    username: "r6p",
                    password: "password",
                    client: "r6p",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parname, paracl FROM pg_parameter_acl ORDER BY parname;",
                    expected: Expected::Rows {
                        columns: &[Column("parname", TEXT), Column("paracl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("datestyle"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                            &[T("log_statement"), T("{postgres=sA/postgres,r6p=s/postgres}")],
                            &[T("my.custom_thing"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                            &[T("timezone"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                            &[T("work_mem"), T("{postgres=sA/postgres,r6p=A*/postgres,r6q=A/r6p}")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6p;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "r6p" cannot be dropped because some objects depend on it"#, detail: "privileges for schema public
privileges for parameter log_statement
privileges for parameter work_mem
owner of function r6f()", ..E }),
                    skip: Some("roles', memberships', parameter grants', and databases' OIDs come from a range of their own, so a listing in OID order puts them after a database's own objects (Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE SET ON PARAMETER log_statement FROM r6p;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALTER SYSTEM ON PARAMETER work_mem FROM r6p;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "dependent privileges exist", hint: "Use CASCADE to revoke them too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALTER SYSTEM ON PARAMETER work_mem FROM r6p CASCADE;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parname, paracl FROM pg_parameter_acl ORDER BY parname;",
                    expected: Expected::Rows {
                        columns: &[Column("parname", TEXT), Column("paracl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("datestyle"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                            &[T("my.custom_thing"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                            &[T("timezone"), T("{postgres=sA/postgres,r6q=sA/postgres}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION r6f();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CREATE ON SCHEMA public FROM r6p;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6p RESET ALL;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6p;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6q;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "r6q" cannot be dropped because some objects depend on it"#, detail: "privileges for parameter timezone
privileges for parameter my.custom_thing
privileges for parameter datestyle", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY r6q;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT parname, paracl FROM pg_parameter_acl ORDER BY parname;",
                    expected: Expected::Rows {
                        columns: &[Column("parname", TEXT), Column("paracl", ACLITEM_ARRAY)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6q;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r6d LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r6e LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, datacl FROM pg_database WHERE datname IN ('postgres', 'template1') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("datacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("postgres"), Null],
                            &[T("template1"), T("{=c/postgres,postgres=CTc/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE, TEMPORARY ON DATABASE postgres TO r6d;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CONNECT ON DATABASE postgres TO r6e WITH GRANT OPTION;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON DATABASE postgres TO r6d;",
                    expected: Expected::Error(Diagnostic { code: "0LP01", message: "invalid privilege type SELECT for database", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CONNECT ON DATABASE nosuchdb TO r6d;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: r#"database "nosuchdb" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT ALL ON DATABASE template1 TO r6d;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, datacl FROM pg_database WHERE datname IN ('postgres', 'template1') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("datacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("postgres"), T("{=Tc/postgres,postgres=CTc/postgres,r6d=CT/postgres,r6e=c*/postgres}")],
                            &[T("template1"), T("{=c/postgres,postgres=CTc/postgres,r6d=CTc/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT has_database_privilege('r6d', 'postgres', 'CREATE'), has_database_privilege('r6e', 'postgres', 'CONNECT WITH GRANT OPTION'), has_database_privilege('r6e', 'postgres', 'TEMP');",
                    expected: Expected::Rows {
                        columns: &[Column("has_database_privilege", BOOL), Column("has_database_privilege", BOOL), Column("has_database_privilege", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CONNECT ON DATABASE postgres FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("r6e")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "r6e",
                    password: "password",
                    client: "r6e",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA r6s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for database postgres", ..E }),
                    username: "r6e",
                    password: "password",
                    client: "r6e",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CONNECT ON DATABASE postgres TO r6d;",
                    expected: Expected::Tag("GRANT"),
                    username: "r6e",
                    password: "password",
                    client: "r6e",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON DATABASE postgres TO r6d;",
                    expected: Expected::Tag("GRANT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "01007", message: r#"no privileges were granted for "postgres""#, ..N }],
                    username: "r6e",
                    password: "password",
                    client: "r6e",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_user;",
                    expected: Expected::Rows {
                        columns: &[Column("current_user", NAME)],
                        rows: &[
                            &[T("r6d")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "r6d",
                    password: "password",
                    client: "r6d",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA r6s;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    username: "r6d",
                    password: "password",
                    client: "r6d",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE r6t (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    username: "r6d",
                    password: "password",
                    client: "r6d",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE r6t;",
                    expected: Expected::Tag("DROP TABLE"),
                    username: "r6d",
                    password: "password",
                    client: "r6d",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6d;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "r6d" cannot be dropped because some objects depend on it"#, detail: "privileges for database template1
privileges for database postgres
owner of schema r6s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6e;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "r6e" cannot be dropped because some objects depend on it"#, detail: "privileges for database postgres", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALL ON DATABASE postgres FROM r6e CASCADE;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY r6d;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, datacl FROM pg_database WHERE datname IN ('postgres', 'template1') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("datacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("postgres"), T("{=T/postgres,postgres=CTc/postgres}")],
                            &[T("template1"), T("{=c/postgres,postgres=CTc/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CONNECT ON DATABASE postgres TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE ALL ON DATABASE template1 FROM r6d;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6d;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6e;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, datacl FROM pg_database WHERE datname IN ('postgres', 'template1') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("datacl", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("postgres"), T("{=Tc/postgres,postgres=CTc/postgres}")],
                            &[T("template1"), T("{=c/postgres,postgres=CTc/postgres}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r6n LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CONNECT ON DATABASE postgres FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Error(Diagnostic { severity: "FATAL", code: "42501", message: r#"permission denied for database "postgres""#, detail: "User does not have CONNECT privilege.", ..E }),
                    username: "r6n",
                    password: "password",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
