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
fn test_event_trigger_ddl() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE, ALTER, and DROP EVENT TRIGGER",
            set_up_script: &[
                "CREATE FUNCTION etf() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'event % %', TG_EVENT, TG_TAG; END $$;",
                "CREATE FUNCTION notet() RETURNS int LANGUAGE sql AS 'SELECT 1';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER et1 ON ddl_command_start EXECUTE FUNCTION etf();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER et2 ON ddl_command_end WHEN TAG IN ('create table', 'DROP TABLE') EXECUTE FUNCTION etf();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER et3 ON sql_drop EXECUTE PROCEDURE etf();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER et4 ON table_rewrite WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION etf();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT evtname, evtevent, evtfoid = 'etf'::regproc, evtenabled, evttags FROM pg_event_trigger ORDER BY evtname;",
                    expected: Expected::Rows {
                        columns: &[Column("evtname", NAME), Column("evtevent", NAME), Column("?column?", BOOL), Column("evtenabled", CHAR), Column("evttags", TEXT_ARRAY)],
                        rows: &[
                            &[T("et1"), T("ddl_command_start"), T("t"), T("O"), Null],
                            &[T("et2"), T("ddl_command_end"), T("t"), T("O"), T(r#"{"CREATE TABLE","DROP TABLE"}"#)],
                            &[T("et3"), T("sql_drop"), T("t"), T("O"), Null],
                            &[T("et4"), T("table_rewrite"), T("t"), T("O"), T(r#"{"ALTER TABLE"}"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER et1 ON ddl_command_start EXECUTE FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"event trigger "et1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON no_such_event EXECUTE FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized event name "no_such_event""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON ddl_command_start WHEN tag IN ('CREATE TABLE') AND tag IN ('DROP TABLE') EXECUTE FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"filter variable "tag" specified more than once"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON ddl_command_start WHEN food IN ('x') EXECUTE FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized filter variable "food""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON ddl_command_start WHEN TAG IN ('SELLECT') EXECUTE FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"filter value "SELLECT" not recognized for filter variable "tag""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON ddl_command_start WHEN TAG IN ('CREATE DATABASE') EXECUTE FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "event triggers are not supported for CREATE DATABASE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON table_rewrite WHEN TAG IN ('CREATE TABLE') EXECUTE FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "event triggers are not supported for CREATE TABLE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON login WHEN TAG IN ('CREATE TABLE') EXECUTE FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "tag filtering is not supported for login event triggers", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON ddl_command_start EXECUTE FUNCTION notet();",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "function notet must return type event_trigger", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER bad ON ddl_command_start EXECUTE FUNCTION nope();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nope() does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER et1 DISABLE;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER et2 ENABLE REPLICA;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER et3 ENABLE ALWAYS;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER nope DISABLE;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"event trigger "nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER et4 RENAME TO et5;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER et5 RENAME TO et1;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"event trigger "et1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT evtname, evtenabled FROM pg_event_trigger ORDER BY evtname;",
                    expected: Expected::Rows {
                        columns: &[Column("evtname", NAME), Column("evtenabled", CHAR)],
                        rows: &[
                            &[T("et1"), T("D")],
                            &[T("et2"), T("R")],
                            &[T("et3"), T("A")],
                            &[T("et5"), T("O")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_event_trigger'::regclass, oid, 0) FROM pg_event_trigger WHERE evtname = 'et1';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("event trigger et1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION etf();",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop function etf() because other objects depend on it", detail: "event trigger et1 depends on function etf()
event trigger et2 depends on function etf()
event trigger et3 depends on function etf()
event trigger et5 depends on function etf()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER nope;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"event trigger "nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER IF EXISTS nope;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    notices: &[Diagnostic { code: "00000", message: r#"event trigger "nope" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER et1, et2, et3, et5;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION etf();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_event_trigger;",
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
            name: "schema changes dolt_merge applies fire no event triggers",
            set_up_script: &[
                "CREATE FUNCTION etf() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'event % %', TG_EVENT, TG_TAG; END $$;",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'other');",
                "CREATE TABLE merged (a INT);",
                "SELECT dolt_commit('-Am', 'add merged');",
                "SELECT dolt_checkout('main');",
                "CREATE EVENT TRIGGER et ON ddl_command_start EXECUTE FUNCTION etf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('other') IS NOT NULL;",
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
    ]);
}

#[test]
fn test_event_trigger_firing() {
    run_scripts(&[
        ScriptTest {
            name: "event triggers fire at ddl_command_start, ddl_command_end, and sql_drop",
            set_up_script: &[
                "CREATE FUNCTION etf() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'event % %', TG_EVENT, TG_TAG; END $$;",
                "CREATE EVENT TRIGGER b_start ON ddl_command_start EXECUTE FUNCTION etf();",
                "CREATE EVENT TRIGGER a_start ON ddl_command_start WHEN TAG IN ('CREATE TABLE') EXECUTE FUNCTION etf();",
                "CREATE EVENT TRIGGER c_end ON ddl_command_end EXECUTE FUNCTION etf();",
                "CREATE EVENT TRIGGER d_drop ON sql_drop EXECUTE FUNCTION etf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (a int PRIMARY KEY, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start CREATE TABLE", ..N }, Diagnostic { code: "00000", message: "event ddl_command_start CREATE TABLE", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end CREATE TABLE", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN c int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start ALTER TABLE", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end ALTER TABLE", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_b ON t (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start CREATE INDEX", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end CREATE INDEX", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT a FROM t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start CREATE VIEW", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end CREATE VIEW", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start DROP VIEW", ..N }, Diagnostic { code: "00000", message: "event sql_drop DROP VIEW", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end DROP VIEW", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start DROP TABLE", ..N }, Diagnostic { code: "00000", message: "event sql_drop DROP TABLE", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end DROP TABLE", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA s CREATE TABLE s.t2 (x int);",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start CREATE SCHEMA", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end CREATE SCHEMA", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA s CASCADE;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start DROP SCHEMA", ..N }, Diagnostic { code: "00000", message: "drop cascades to table s.t2", ..N }, Diagnostic { code: "00000", message: "event sql_drop DROP SCHEMA", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end DROP SCHEMA", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER b_start DISABLE;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start CREATE TABLE", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end CREATE TABLE", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t4 (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER c_end ENABLE REPLICA;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t5 (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_end CREATE TABLE", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET session_replication_role;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET event_triggers = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t6 (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t6;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET event_triggers;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION eterr() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'no ddl %', TG_TAG; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER e_err ON ddl_command_start WHEN TAG IN ('CREATE SEQUENCE') EXECUTE FUNCTION eterr();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE sq;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "no ddl CREATE SEQUENCE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'sq';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[&[T("0")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "event_triggers = off keeps every event trigger from firing",
            set_up_script: &[
                "CREATE FUNCTION etf() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'event % %', TG_EVENT, TG_TAG; END $$;",
                "CREATE EVENT TRIGGER et_start ON ddl_command_start EXECUTE FUNCTION etf();",
                "CREATE EVENT TRIGGER et_end ON ddl_command_end EXECUTE FUNCTION etf();",
                "CREATE EVENT TRIGGER et_drop ON sql_drop EXECUTE FUNCTION etf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET event_triggers = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET event_triggers;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start CREATE TABLE", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end CREATE TABLE", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start DROP TABLE", ..N }, Diagnostic { code: "00000", message: "event sql_drop DROP TABLE", ..N }, Diagnostic { code: "00000", message: "event ddl_command_end DROP TABLE", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE regress_evt_user;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_evt_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET event_triggers = off;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to set parameter "event_triggers""#, ..E }),
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
        ScriptTest {
            name: "a statement rerun after a lock wait found the catalog changed fires its ddl_command_start triggers' notices once",
            set_up_script: &[
                "CREATE TABLE t (a int);",
                "CREATE FUNCTION etf() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'event % %', TG_EVENT, TG_TAG; END $$;",
                "CREATE EVENT TRIGGER et_start ON ddl_command_start WHEN TAG IN ('CREATE INDEX') EXECUTE FUNCTION etf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN b int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_a ON t (a);",
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass FROM pg_index WHERE indrelid = 't'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS)],
                        rows: &[&[T("t_a")]],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "event ddl_command_start CREATE INDEX", ..N }],
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_event_trigger_collections() {
    run_scripts(&[
        ScriptTest {
            name: "pg_event_trigger_ddl_commands and pg_event_trigger_dropped_objects",
            set_up_script: &[
                "CREATE FUNCTION ddl_end() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_ddl_commands() LOOP RAISE NOTICE 'end: % % % % %', r.command_tag, r.object_type, r.schema_name, r.object_identity, r.in_extension; END LOOP; END $$;",
                "CREATE FUNCTION on_drop() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_dropped_objects() LOOP RAISE NOTICE 'drop: % % % % % % % % %', r.original, r.normal, r.is_temporary, r.object_type, r.schema_name, r.object_name, r.object_identity, r.address_names, r.address_args; END LOOP; END $$;",
                "CREATE EVENT TRIGGER e1 ON ddl_command_end EXECUTE FUNCTION ddl_end();",
                "CREATE EVENT TRIGGER e2 ON sql_drop EXECUTE FUNCTION on_drop();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (a serial PRIMARY KEY, b int DEFAULT 1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[
                        Diagnostic { code: "00000", message: "end: CREATE SEQUENCE sequence public public.t_a_seq f", ..N },
                        Diagnostic { code: "00000", message: "end: CREATE TABLE table public public.t f", ..N },
                        Diagnostic { code: "00000", message: "end: CREATE INDEX index public public.t_pkey f", ..N },
                        Diagnostic { code: "00000", message: "end: ALTER SEQUENCE sequence public public.t_a_seq f", ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN c int, ALTER COLUMN b SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[
                        Diagnostic { code: "00000", message: "end: ALTER TABLE table public public.t f", ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_c ON t (c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    notices: &[
                        Diagnostic { code: "00000", message: "end: CREATE INDEX index public public.t_c f", ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT a, b FROM t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    notices: &[
                        Diagnostic { code: "00000", message: "end: CREATE VIEW view public public.v f", ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA s CREATE TABLE s.t2 (x int);",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    notices: &[
                        Diagnostic { code: "00000", message: "end: CREATE SCHEMA schema <NULL> s f", ..N },
                        Diagnostic { code: "00000", message: "end: CREATE TABLE table s s.t2 f", ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION s.f(int) RETURNS int LANGUAGE sql AS 'SELECT $1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    notices: &[
                        Diagnostic { code: "00000", message: "end: CREATE FUNCTION function s s.f(integer) f", ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP COLUMN c;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[
                        Diagnostic { code: "00000", message: "drop: t f f table column public <NULL> public.t.c {public,t,c} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f index public t_c public.t_c {public,t_c} {}", ..N },
                        Diagnostic { code: "00000", message: "end: ALTER TABLE table public public.t f", ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t CASCADE;",
                    expected: Expected::Tag("DROP TABLE"),
                    notices: &[
                        Diagnostic { code: "00000", message: "drop cascades to view v", ..N },
                        Diagnostic { code: "00000", message: "drop: t f f table public t public.t {public,t} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f sequence public t_a_seq public.t_a_seq {public,t_a_seq} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f type public t public.t {public.t} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f type public _t public.t[] {public.t[]} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f t f default value public <NULL> for public.t.a {public,t,a} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f default value public <NULL> for public.t.b {public,t,b} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f table constraint public <NULL> t_a_not_null on public.t {public,t,t_a_not_null} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f table constraint public <NULL> t_pkey on public.t {public,t,t_pkey} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f index public t_pkey public.t_pkey {public,t_pkey} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f table constraint public <NULL> t_b_not_null on public.t {public,t,t_b_not_null} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f t f view public v public.v {public,v} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f type public v public.v {public.v} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f type public _v public.v[] {public.v[]} {}", ..N },
                        Diagnostic { code: "00000", message: r#"drop: f t f rule <NULL> <NULL> "_RETURN" on public.v {public,v,_RETURN} {}"#, ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA s CASCADE;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    notices: &[
                        Diagnostic { code: "00000", message: "drop cascades to 2 other objects", detail: "drop cascades to table s.t2\ndrop cascades to function s.f(integer)", ..N },
                        Diagnostic { code: "00000", message: "drop: t f f schema <NULL> s s {s} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f t f table s t2 s.t2 {s,t2} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f type s t2 s.t2 {s.t2} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f f f type s _t2 s.t2[] {s.t2[]} {}", ..N },
                        Diagnostic { code: "00000", message: "drop: f t f function s <NULL> s.f(integer) {s,f} {integer}", ..N },
                    ],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_event_trigger_ddl_commands();",
                    expected: Expected::Error(Diagnostic { code: "39P03", message: "pg_event_trigger_ddl_commands() can only be called in an event trigger function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_event_trigger_dropped_objects();",
                    expected: Expected::Error(Diagnostic { code: "39P03", message: "pg_event_trigger_dropped_objects() can only be called in a sql_drop event trigger function", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_table_rewrite_event_triggers() {
    run_scripts(&[
        ScriptTest {
            name: "table_rewrite event triggers fire before ALTER TABLE rewrites a table, with its table and reason",
            set_up_script: &[
                "CREATE TABLE rw (a INT);",
                "INSERT INTO rw VALUES (1), (2);",
                "CREATE FUNCTION rw_log() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'rewriting table % for reason %', pg_event_trigger_table_rewrite_oid()::regclass, pg_event_trigger_table_rewrite_reason(); END $$;",
                "CREATE EVENT TRIGGER rw_et ON table_rewrite EXECUTE FUNCTION rw_log();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE rw ADD COLUMN b INT DEFAULT (random() * 0)::INT;",
                    notices: &[Diagnostic { code: "00000", message: "rewriting table rw for reason 2", ..N }],
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rw ADD COLUMN c INT DEFAULT 5;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rw SET UNLOGGED;",
                    notices: &[Diagnostic { code: "00000", message: "rewriting table rw for reason 1", ..N }],
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, c FROM rw ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("0"), T("5")],
                            &[T("2"), T("0"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_event_trigger_table_rewrite_oid();",
                    expected: Expected::Error(Diagnostic { code: "39P03", message: "pg_event_trigger_table_rewrite_oid() can only be called in a table_rewrite event trigger function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_event_trigger_table_rewrite_reason();",
                    expected: Expected::Error(Diagnostic { code: "39P03", message: "pg_event_trigger_table_rewrite_reason() can only be called in a table_rewrite event trigger function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION rw_forbid() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'rewrites of % are not allowed', pg_event_trigger_table_rewrite_oid()::regclass; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER rw_no ON table_rewrite WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION rw_forbid();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rw SET LOGGED;",
                    notices: &[Diagnostic { code: "00000", message: "rewriting table rw for reason 1", ..N }],
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "rewrites of rw are not allowed", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER rw_no DISABLE;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rw SET LOGGED;",
                    notices: &[Diagnostic { code: "00000", message: "rewriting table rw for reason 1", ..N }],
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_login_event_triggers() {
    run_scripts(&[
        ScriptTest {
            name: "login event triggers fire as each session starts, and dathasloginevt clears once none is enabled",
            database: "login_triggers",
            set_up_script: &[
                "CREATE TABLE login_log (who NAME);",
                "CREATE FUNCTION log_login() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO login_log VALUES (session_user); END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER log_logins ON login EXECUTE FUNCTION log_login();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dathasloginevt FROM pg_database WHERE datname = current_database();",
                    expected: Expected::Rows { columns: &[Column("dathasloginevt", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM login_log;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER log_logins DISABLE;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dathasloginevt FROM pg_database WHERE datname = current_database();",
                    expected: Expected::Rows { columns: &[Column("dathasloginevt", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM login_log;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dathasloginevt FROM pg_database WHERE datname = current_database();",
                    expected: Expected::Rows { columns: &[Column("dathasloginevt", BOOL)], rows: &[&[T("f")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EVENT TRIGGER log_logins ENABLE;",
                    expected: Expected::Tag("ALTER EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dathasloginevt FROM pg_database WHERE datname = current_database();",
                    expected: Expected::Rows { columns: &[Column("dathasloginevt", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM login_log;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    client: "d",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER log_logins;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM login_log;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    client: "e",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER log_logins;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dathasloginevt FROM pg_database WHERE datname = current_database();",
                    expected: Expected::Rows { columns: &[Column("dathasloginevt", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM login_log;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    client: "f",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dathasloginevt FROM pg_database WHERE datname = current_database();",
                    expected: Expected::Rows { columns: &[Column("dathasloginevt", BOOL)], rows: &[&[T("f")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER log_logins ON login EXECUTE FUNCTION log_login();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dathasloginevt FROM pg_database WHERE datname = current_database();",
                    expected: Expected::Rows { columns: &[Column("dathasloginevt", BOOL)], rows: &[&[T("f")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_of_a_transaction_that_changed_it() {
    run_scripts(&[
        ScriptTest {
            name: "a rollback to a savepoint after DDL shows the catalog as it was at the savepoint",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE cached_a (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE cached_b (b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM cached_b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM cached_a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM cached_b;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "cached_b" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a transaction that changed the catalog still sees DDL other sessions commit",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE own_a (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM own_a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE theirs (t INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t FROM theirs;",
                    expected: Expected::Rows {
                        columns: &[Column("t", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    client: "A",
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
    ]);
}

#[test]
fn test_ddl_command_start_fires_once_when_a_statement_runs_again() {
    run_scripts(&[
        ScriptTest {
            name: "the first temporary table of a session fires ddl_command_start once",
            set_up_script: &[
                "CREATE SEQUENCE start_calls;",
                "CREATE FUNCTION start_once() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE c refcursor := 'start_cursor'; BEGIN RAISE NOTICE 'start % %', TG_TAG, nextval('start_calls'); OPEN c FOR SELECT 1; END $$;",
                "CREATE EVENT TRIGGER start_once ON ddl_command_start WHEN TAG IN ('CREATE TABLE') EXECUTE FUNCTION start_once();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE start_temp (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "start CREATE TABLE 1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT currval('start_calls');",
                    expected: Expected::Rows {
                        columns: &[Column("currval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH start_cursor;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
