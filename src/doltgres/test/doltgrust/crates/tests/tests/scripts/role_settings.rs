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
use harness::script::{A, Column, Diagnostic, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_role_settings() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER ROLE SET, ALTER DATABASE SET, and the settings a login gets",
            set_up_script: &[
                "CREATE ROLE r6a LOGIN PASSWORD 'password';",
                "CREATE ROLE r6b LOGIN PASSWORD 'password' CREATEROLE;",
                "CREATE ROLE r6su LOGIN PASSWORD 'password' SUPERUSER;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET work_mem = '2MB';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a IN DATABASE postgres SET search_path = 'a, public';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE postgres SET statement_timeout = '5s';",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE ALL SET lock_timeout = '7s';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE ALL IN DATABASE postgres SET lock_timeout = '8s';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname, r.rolname, s.setconfig FROM pg_db_role_setting s LEFT JOIN pg_database d ON d.oid = s.setdatabase LEFT JOIN pg_roles r ON r.oid = s.setrole ORDER BY 1 NULLS FIRST, 2 NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("rolname", NAME), Column("setconfig", TEXT_ARRAY)],
                        rows: &[
                            &[Null, Null, T("{lock_timeout=7s}")],
                            &[Null, T("r6a"), T("{work_mem=2MB}")],
                            &[T("postgres"), Null, T("{statement_timeout=5s,lock_timeout=8s}")],
                            &[T("postgres"), T("r6a"), T(r#"{"search_path=\"a, public\""}"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET work_mem = '3MB';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET enable_seqscan TO off;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET timezone = 'America/Los_Angeles';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET timezone FROM CURRENT;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setconfig FROM pg_db_role_setting WHERE setrole = 'r6a'::regrole AND setdatabase = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("setconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("{work_mem=3MB,enable_seqscan=off,TimeZone=America/Los_Angeles}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a RESET enable_seqscan;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a RESET nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized configuration parameter "nosuch""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET nosuch = 1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"unrecognized configuration parameter "nosuch""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET my.custom = 'x';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET work_mem = 'abc';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "work_mem": "abc""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET log_statement = 'all';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setconfig FROM pg_db_role_setting WHERE setrole = 'r6a'::regrole AND setdatabase = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("setconfig", TEXT_ARRAY)],
                        rows: &[
                            &[T("{work_mem=3MB,TimeZone=America/Los_Angeles,my.custom=x,log_statement=all}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a IN DATABASE nosuchdb SET work_mem = '1MB';",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: r#"database "nosuchdb" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE nosuchrole SET work_mem = '1MB';",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuchrole" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE nosuchdb SET work_mem = '1MB';",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: r#"database "nosuchdb" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER USER r6a SET maintenance_work_mem = '64MB';",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW work_mem;",
                    expected: Expected::Rows {
                        columns: &[Column("work_mem", TEXT)],
                        rows: &[
                            &[T("3MB")],
                        ],
                        tag: "SHOW",
                    },
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW search_path;",
                    expected: Expected::Rows {
                        columns: &[Column("search_path", TEXT)],
                        rows: &[
                            &[T(r#""a, public""#)],
                        ],
                        tag: "SHOW",
                    },
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW statement_timeout;",
                    expected: Expected::Rows {
                        columns: &[Column("statement_timeout", TEXT)],
                        rows: &[
                            &[T("5s")],
                        ],
                        tag: "SHOW",
                    },
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW lock_timeout;",
                    expected: Expected::Rows {
                        columns: &[Column("lock_timeout", TEXT)],
                        rows: &[
                            &[T("8s")],
                        ],
                        tag: "SHOW",
                    },
                    username: "r6a",
                    password: "password",
                    client: "r6a",
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
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, setting, source FROM pg_settings WHERE name IN ('work_mem', 'search_path', 'statement_timeout', 'lock_timeout', 'maintenance_work_mem', 'timezone') ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("setting", TEXT), Column("source", TEXT)],
                        rows: &[
                            &[T("lock_timeout"), T("8000"), T("database")],
                            &[T("maintenance_work_mem"), T("65536"), T("user")],
                            &[T("search_path"), T(r#""a, public""#), T("database user")],
                            &[T("statement_timeout"), T("5000"), T("database")],
                            &[T("work_mem"), T("3072"), T("user")],
                        ],
                        tag: "SELECT 5",
                    },
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW work_mem;",
                    expected: Expected::Rows {
                        columns: &[Column("work_mem", TEXT)],
                        rows: &[
                            &[T("3MB")],
                        ],
                        tag: "SHOW",
                    },
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET work_mem = '4MB';",
                    expected: Expected::Tag("ALTER ROLE"),
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET log_statement = 'none';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set parameter "log_statement""#, ..E }),
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6b SET work_mem = '4MB';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter role", detail: r#"Only roles with the CREATEROLE attribute and the ADMIN option on role "r6b" may alter this role."#, ..E }),
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE ALL SET work_mem = '4MB';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter setting", detail: "Only roles with the SUPERUSER attribute may alter settings globally.", ..E }),
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE postgres SET work_mem = '4MB';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of database postgres", ..E }),
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a RESET ALL;",
                    expected: Expected::Tag("ALTER ROLE"),
                    username: "r6a",
                    password: "password",
                    client: "r6a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET work_mem = '5MB';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter role", detail: r#"Only roles with the CREATEROLE attribute and the ADMIN option on role "r6a" may alter this role."#, ..E }),
                    username: "r6b",
                    password: "password",
                    client: "r6b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6su SET work_mem = '5MB';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to alter role", detail: "Only roles with the SUPERUSER attribute may alter roles with the SUPERUSER attribute.", ..E }),
                    username: "r6b",
                    password: "password",
                    client: "r6b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT r6a TO r6b WITH ADMIN OPTION;",
                    expected: Expected::Tag("GRANT ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a SET work_mem = '5MB';",
                    expected: Expected::Tag("ALTER ROLE"),
                    username: "r6b",
                    password: "password",
                    client: "r6b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname, r.rolname, s.setconfig FROM pg_db_role_setting s LEFT JOIN pg_database d ON d.oid = s.setdatabase LEFT JOIN pg_roles r ON r.oid = s.setrole ORDER BY 1 NULLS FIRST, 2 NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("rolname", NAME), Column("setconfig", TEXT_ARRAY)],
                        rows: &[
                            &[Null, Null, T("{lock_timeout=7s}")],
                            &[Null, T("r6a"), T("{my.custom=x,log_statement=all,work_mem=5MB}")],
                            &[T("postgres"), Null, T("{statement_timeout=5s,lock_timeout=8s}")],
                            &[T("postgres"), T("r6a"), T(r#"{"search_path=\"a, public\""}"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE r6a IN DATABASE postgres RESET ALL;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE postgres RESET ALL;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE ALL RESET ALL;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE ALL IN DATABASE postgres RESET lock_timeout;",
                    expected: Expected::Tag("ALTER ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6a;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.datname, r.rolname, s.setconfig FROM pg_db_role_setting s LEFT JOIN pg_database d ON d.oid = s.setdatabase LEFT JOIN pg_roles r ON r.oid = s.setrole ORDER BY 1 NULLS FIRST, 2 NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("rolname", NAME), Column("setconfig", TEXT_ARRAY)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6b;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r6su;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
