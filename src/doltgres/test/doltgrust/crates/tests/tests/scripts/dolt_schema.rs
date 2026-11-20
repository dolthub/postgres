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

//! Tests of the reserved schema `dolt`, whose names of Dolt's system tables without their `dolt_` prefix name
//! pg_catalog's relations of those tables.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_schema() {
    run_scripts(&[
        ScriptTest {
            name: "the schema dolt names Dolt's system tables without their prefix, as the same relations",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY)",
                "SELECT dolt_commit('-Am', 'one')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'dolt.log'::regclass, 'dolt.log'::regclass::oid = 'dolt_log'::regclass::oid;",
                    expected: Expected::Rows {
                        columns: &[Column("regclass", REGCLASS), Column("?column?", BOOL)],
                        rows: &[
                            &[T("dolt_log"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt.log ORDER BY date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_regclass('dolt.status')::oid = 'pg_catalog.dolt_status'::regclass::oid, to_regclass('dolt.nope'), to_regclass('dolt.dolt_log');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("to_regclass", REGCLASS), Column("to_regclass", REGCLASS)],
                        rows: &[
                            &[T("t"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM log;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"log\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relnamespace = 'dolt'::regnamespace;",
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
                    query: "SELECT c.oid FROM pg_catalog.pg_class c LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE c.relname OPERATOR(pg_catalog.~) '^(status)$' COLLATE pg_catalog.default AND n.nspname OPERATOR(pg_catalog.~) '^(dolt)$' COLLATE pg_catalog.default;",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT table_name FROM dolt.status;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_viewdef('v');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_viewdef", TEXT)],
                        rows: &[
                            &[T(" SELECT table_name\n   FROM dolt_status;")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path TO dolt;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) > 0 FROM log;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "the schema dolt can't be made, renamed, dropped, or given objects",
            set_up_script: &[
                "CREATE ROLE dolt",
                "CREATE TABLE y (a INT)",
                "CREATE FUNCTION f() RETURNS INT LANGUAGE sql AS 'SELECT 1'",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE SCHEMA dolt;",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "unacceptable schema name \"dolt\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA IF NOT EXISTS dolt;",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "unacceptable schema name \"dolt\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA AUTHORIZATION dolt;",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "unacceptable schema name \"dolt\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA public RENAME TO dolt;",
                    expected: Expected::Error(Diagnostic { code: "42P06", message: "schema \"dolt\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA dolt RENAME TO d2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to rename schema \"dolt\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dolt.x (a INT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create \"dolt.x\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", position: 14, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION dolt.f() RETURNS INT LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create \"dolt.f\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE y SET SCHEMA dolt;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create \"dolt.y\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION f() SET SCHEMA dolt;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to move objects into schema \"dolt\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA dolt;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop schema dolt because it is required by the database system", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA dolt CASCADE;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop schema dolt because it is required by the database system", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path TO dolt;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE z (a INT);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create \"dolt.z\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", position: 14, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create \"dolt.s\"", detail: "The schema \"dolt\" is reserved for Dolt's system tables.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "the schema dolt is listed only while dolt_show_system_tables is on, and found by name either way",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT nspname FROM pg_namespace WHERE nspname = 'dolt';",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM information_schema.schemata WHERE schema_name = 'dolt';",
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
                    query: "SELECT has_schema_privilege('dolt', 'USAGE'), to_regnamespace('dolt') IS NOT NULL, (SELECT count(*) FROM dolt.status);",
                    expected: Expected::Rows {
                        columns: &[Column("has_schema_privilege", BOOL), Column("?column?", BOOL), Column("count", INT8)],
                        rows: &[
                            &[T("t"), T("t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_show_system_tables = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspname, nspowner::regrole FROM pg_namespace WHERE nspname = 'dolt';",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME), Column("nspowner", REGROLE)],
                        rows: &[
                            &[T("dolt"), T("postgres")],
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
