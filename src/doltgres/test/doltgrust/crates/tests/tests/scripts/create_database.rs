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
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_create_database() {
    run_scripts(&[
        ScriptTest {
            name: "simple create database",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE testdb",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "USE testdb",
                    expected: Expected::Tag("SET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "encoding",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE testdb encoding=utf8",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "USE testdb",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "CREATE DATABASE testdb2 encoding=latin1",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "USE testdb2",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "CREATE DATABASE testdb3 encoding=notexist",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple options",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE testdb OWNER=foo ENCODING=utf8",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, and the failed CREATE DATABASE above creates nothing, as in Postgres.
                ScriptTestAssertion {
                    query: "USE testdb",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: r#"database "testdb" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "with hyphen",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE "test-db""#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: r#"USE "test-db""#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_database() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER DATABASE changes a database's options and name in its transaction, and counts the change of pg_database",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE ad_db;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db CONNECTION LIMIT 5;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datconnlimit, datallowconn, datistemplate FROM pg_database WHERE datname = 'ad_db';",
                    expected: Expected::Rows {
                        columns: &[Column("datconnlimit", INT4), Column("datallowconn", BOOL), Column("datistemplate", BOOL)],
                        rows: &[
                            &[T("5"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db WITH ALLOW_CONNECTIONS false IS_TEMPLATE true;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datconnlimit, datallowconn, datistemplate FROM pg_database WHERE datname = 'ad_db';",
                    expected: Expected::Rows {
                        columns: &[Column("datconnlimit", INT4), Column("datallowconn", BOOL), Column("datistemplate", BOOL)],
                        rows: &[
                            &[T("5"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db ALLOW_CONNECTIONS true IS_TEMPLATE false CONNECTION LIMIT -1;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datconnlimit, datallowconn, datistemplate FROM pg_database WHERE datname = 'ad_db';",
                    expected: Expected::Rows {
                        columns: &[Column("datconnlimit", INT4), Column("datallowconn", BOOL), Column("datistemplate", BOOL)],
                        rows: &[
                            &[T("-1"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db CONNECTION LIMIT -2;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid connection limit: -2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db CONNECTION LIMIT 1 CONNECTION LIMIT 2;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "conflicting or redundant options", position: 41, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db WITH foo 1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "option \"foo\" not recognized", position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db TABLESPACE pg_default CONNECTION LIMIT 3;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "option \"tablespace\" cannot be specified with other options", position: 22, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE nosuch CONNECTION LIMIT 3;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('ALTER DATABASE %I ALLOW_CONNECTIONS false', current_database()); END $$;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot disallow connections for current database", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db RENAME TO ad_db2;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname LIKE 'ad_db%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("ad_db2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db2 RENAME TO postgres;",
                    expected: Expected::Error(Diagnostic { code: "42P04", message: "database \"postgres\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE nosuch RENAME TO nosuch2;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('ALTER DATABASE %I RENAME TO ad_current', current_database()); END $$;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "current database cannot be renamed", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db2 RENAME TO ad_db3;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname LIKE 'ad_db%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("ad_db2")],
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
                    query: "ALTER DATABASE ad_db2 RENAME TO ad_db4;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db4 CONNECTION LIMIT 7;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, datconnlimit FROM pg_database WHERE datname LIKE 'ad_db%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("datconnlimit", INT4)],
                        rows: &[
                            &[T("ad_db4"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE ad_db4 RESET TABLESPACE;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER DATABASE ad_db4 CONNECTION LIMIT 8;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd <= 1 AS hot_bounded FROM pg_stat_xact_sys_tables WHERE relname = 'pg_database';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("hot_bounded", BOOL)],
                        rows: &[
                            &[T("pg_database"), T("0"), T("1"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "ALTER DATABASE ad_db4 RENAME TO ad_db5;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, n_tup_ins, n_tup_upd, n_tup_del, n_tup_hot_upd FROM pg_stat_xact_sys_tables WHERE relname = 'pg_database';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("n_tup_ins", INT8), Column("n_tup_upd", INT8), Column("n_tup_del", INT8), Column("n_tup_hot_upd", INT8)],
                        rows: &[
                            &[T("pg_database"), T("0"), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE ad_db5;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_database_refresh_collation_version() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER DATABASE ... REFRESH COLLATION VERSION records the version the provider gives now, as a change its transaction can roll back",
            set_up_script: &[
                "CREATE DATABASE refcoll TEMPLATE template0 LOCALE_PROVIDER builtin BUILTIN_LOCALE 'C.UTF-8' COLLATION_VERSION '0.9';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datcollversion FROM pg_database WHERE datname = 'refcoll';",
                    expected: Expected::Rows {
                        columns: &[Column("datcollversion", TEXT)],
                        rows: &[
                            &[T("0.9")],
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
                    query: "ALTER DATABASE refcoll REFRESH COLLATION VERSION;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    notices: &[Diagnostic { severity: "NOTICE", code: "00000", message: "changing version from 0.9 to 1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datcollversion FROM pg_database WHERE datname = 'refcoll';",
                    expected: Expected::Rows {
                        columns: &[Column("datcollversion", TEXT)],
                        rows: &[
                            &[T("1")],
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
                    query: "SELECT datcollversion FROM pg_database WHERE datname = 'refcoll';",
                    expected: Expected::Rows {
                        columns: &[Column("datcollversion", TEXT)],
                        rows: &[
                            &[T("0.9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE refcoll REFRESH COLLATION VERSION;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    notices: &[Diagnostic { severity: "NOTICE", code: "00000", message: "changing version from 0.9 to 1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datcollversion FROM pg_database WHERE datname = 'refcoll';",
                    expected: Expected::Rows {
                        columns: &[Column("datcollversion", TEXT)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE refcoll REFRESH COLLATION VERSION;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    notices: &[Diagnostic { severity: "NOTICE", code: "00000", message: "version has not changed", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DATABASE nosuchdb REFRESH COLLATION VERSION;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: r#"database "nosuchdb" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE refc3 TEMPLATE refcoll;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE refc3;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE refcoll;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_create_database_reads_its_template_as_last_altered() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE DATABASE judges its template by the registry entry as the last committed ALTER DATABASE left it",
            set_up_script: &[
                "CREATE ROLE tmplr CREATEDB;",
                "CREATE DATABASE tmpl1 TEMPLATE template0;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER DATABASE tmpl1 IS_TEMPLATE true;",
                    expected: Expected::Tag("ALTER DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE tmplr;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE tmplcopy TEMPLATE tmpl1;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE tmplcopy;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
