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
fn test_alter_statements() {
    run_scripts(&[
        ScriptTest {
            name: "alter database",
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER DATABASE postgres OWNER TO foo",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter sequence",
            set_up_script: &[
                "CREATE SEQUENCE testseq",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER SEQUENCE testseq OWNER TO foo",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter type",
            set_up_script: &[
                "CREATE TYPE testtype AS ENUM ('a', 'b', 'c')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TYPE testtype OWNER TO foo",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter function",
            set_up_script: &[
                "CREATE FUNCTION testfunc() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER FUNCTION testfunc() OWNER TO foo",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter procedure",
            set_up_script: &[
                "CREATE TABLE test (v1 INT8);",
                "CREATE PROCEDURE testproc() AS $$ BEGIN INSERT INTO test VALUES (1); END; $$ LANGUAGE plpgsql;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER PROCEDURE testproc() OWNER TO foo;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter schema",
            set_up_script: &[
                "CREATE SCHEMA testschema",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER schema testschema OWNER TO foo",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter view",
            set_up_script: &[
                "CREATE VIEW testview AS SELECT 1",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER VIEW testview OWNER TO foo",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "foo" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "OWNER TO checks the role before the object",
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER DATABASE alt_nope OWNER TO alt_nobody;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "alt_nobody" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE alt_nope OWNER TO alt_nobody;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "alt_nobody" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA alt_nope OWNER TO alt_nobody;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "alt_nobody" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER VIEW alt_nope OWNER TO alt_nobody;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "alt_nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION alt_nope() OWNER TO alt_nobody;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "alt_nobody" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA alt_nope OWNER TO postgres;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: r#"schema "alt_nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE alt_nope OWNER TO postgres;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "alt_nope" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_owner_to_relations() {
    run_scripts(&[
        ScriptTest {
            name: "owner to on sequences, views, and predefined roles",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE r1;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE s1;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v1 AS SELECT * FROM t1;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE s1 OWNER TO r1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE v1 OWNER TO r1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE public.s1 OWNER TO postgres;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA public OWNER TO pg_database_owner;",
                    expected: Expected::Tag("ALTER SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION IF NOT EXISTS plpgsql WITH SCHEMA pg_catalog;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    notices: &[Diagnostic { code: "42710", message: r#"extension "plpgsql" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION plpgsql;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"extension "plpgsql" already exists"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SEQUENCE s1 OWNER TO r1;",
                    expected: Expected::Tag("ALTER SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER VIEW v1 OWNER TO r1;",
                    expected: Expected::Tag("ALTER VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SEQUENCE v1 OWNER TO r1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""v1" is not a sequence"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER VIEW s1 OWNER TO r1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""s1" is not a view"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET statement_timeout = -1;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"-1 ms is outside the valid range for parameter "statement_timeout" (0 ms .. 2147483647 ms)"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '-5s';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"-5000 ms is outside the valid range for parameter "lock_timeout" (0 ms .. 2147483647 ms)"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = 10;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"10 kB is outside the valid range for parameter "work_mem" (64 kB .. 2147483647 kB)"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v1;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SEQUENCE s1;",
                    expected: Expected::Tag("DROP SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE r1;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_add_enum_column_default() {
    run_scripts(&[
        ScriptTest {
            name: "enum default controls",
            set_up_script: &[
                "CREATE TYPE enum_default_probe AS ENUM ('x');",
                "CREATE TABLE enum_default_no_default_control (id integer);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE enum_default_create_control (value enum_default_probe DEFAULT 'x');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO enum_default_create_control DEFAULT VALUES;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT value FROM enum_default_create_control;",
                    expected: Expected::Rows {
                        columns: &[Column("value", USER_DEFINED)],
                        rows: &[
                            &[T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE enum_default_no_default_control ADD COLUMN value enum_default_probe;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "add enum column with string literal default",
            set_up_script: &[
                "CREATE TYPE enum_default_probe AS ENUM ('x');",
                "CREATE TABLE enum_default_probe_table (id integer);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE enum_default_probe_table ADD COLUMN value enum_default_probe DEFAULT 'x';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO enum_default_probe_table (id) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, value FROM enum_default_probe_table;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "add schema qualified enum column with not null default",
            set_up_script: &[
                "CREATE SCHEMA auth;",
                "CREATE TYPE auth.oauth_client_type AS ENUM ('confidential', 'public');",
                "CREATE TABLE auth.oauth_clients (id integer);",
                "INSERT INTO auth.oauth_clients VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE auth.oauth_clients ADD COLUMN IF NOT EXISTS client_type auth.oauth_client_type NOT NULL DEFAULT 'confidential';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, client_type FROM auth.oauth_clients;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("client_type", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("confidential")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO auth.oauth_clients (id) VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, client_type FROM auth.oauth_clients ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("client_type", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("confidential")],
                            &[T("2"), T("confidential")],
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
fn test_rename_and_move_built_in_objects() {
    run_scripts(&[
        ScriptTest {
            name: "renaming and moving built-in objects frees their old names and finds them by their new ones",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER SCHEMA public RENAME TO pub2;",
                    expected: Expected::Tag("ALTER SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspname FROM pg_namespace WHERE oid = 2200;",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME)],
                        rows: &[
                            &[T("pub2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pub2.t (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA public;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_namespace WHERE nspname IN ('public', 'pub2');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION now() RENAME TO now2;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT now2() IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT now();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: r#"function now() does not exist"#, hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pg_catalog.now() RETURNS TIMESTAMPTZ LANGUAGE sql AS 'SELECT now2()';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION now2() SET SCHEMA public;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pronamespace::regnamespace FROM pg_proc WHERE proname = 'now2';",
                    expected: Expected::Rows {
                        columns: &[Column("pronamespace", REGNAMESPACE)],
                        rows: &[
                            &[T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION length(text) RENAME TO upper;",
                    expected: Expected::Error(Diagnostic { code: "42723", message: r#"function upper(text) already exists in schema "pg_catalog""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE int4 RENAME TO int4b;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1::int4b;",
                    expected: Expected::Rows {
                        columns: &[Column("int4b", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE pg_catalog.int4b RENAME TO int4;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER LANGUAGE plpgsql RENAME TO plpgsql2;",
                    expected: Expected::Tag("ALTER LANGUAGE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lanname FROM pg_language WHERE lanname LIKE 'plpgsql%';",
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME)],
                        rows: &[
                            &[T("plpgsql2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR FAMILY integer_ops USING btree RENAME TO integer_ops2;",
                    expected: Expected::Tag("ALTER OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION \"C\" RENAME TO c2;",
                    expected: Expected::Tag("ALTER COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION \"POSIX\" SET SCHEMA public;",
                    expected: Expected::Tag("ALTER COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT collnamespace::regnamespace FROM pg_collation WHERE collname = 'POSIX';",
                    expected: Expected::Rows {
                        columns: &[Column("collnamespace", REGNAMESPACE)],
                        rows: &[
                            &[T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pg_catalog.pg_class RENAME TO pg_class2;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied: "pg_class" is a system catalog"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER VIEW pg_catalog.pg_tables RENAME TO pg_tables2;",
                    expected: Expected::Tag("ALTER VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_class WHERE oid = 'pg_catalog.pg_tables2'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("pg_tables2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SCHEMA information_schema RENAME TO is2;",
                    expected: Expected::Tag("ALTER SCHEMA"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
