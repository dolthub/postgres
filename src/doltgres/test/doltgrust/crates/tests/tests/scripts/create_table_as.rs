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
fn test_create_table_as_and_select_into() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TABLE AS and SELECT ... INTO: columns, rows, WITH NO DATA, IF NOT EXISTS, TEMP, rollback, and EXPLAIN",
            set_up_script: &[
                "CREATE TABLE src (a int, b varchar(10), c numeric(8,2), d text COLLATE \"C\");",
                "INSERT INTO src VALUES (1, 'x', 1.5, 'p'), (2, 'y', 2.25, 'q'), (3, NULL, NULL, 'r');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 AS SELECT * FROM src WHERE a < 3;",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", VARCHAR), Column("c", NUMERIC), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("x"), T("1.50"), T("p")],
                            &[T("2"), T("y"), T("2.25"), T("q")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, format_type(atttypid, atttypmod), attnotnull, (SELECT collname FROM pg_collation WHERE oid = attcollation) AS coll FROM pg_attribute WHERE attrelid = 't1'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("coll", NAME)],
                        rows: &[
                            &[T("a"), T("integer"), T("f"), Null],
                            &[T("b"), T("character varying(10)"), T("f"), T("default")],
                            &[T("c"), T("numeric(8,2)"), T("f"), Null],
                            &[T("d"), T("text"), T("f"), T("C")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (x, y) AS SELECT a, b, 'lit', 1 + 1 AS two FROM src;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, format_type(atttypid, atttypmod), (SELECT collname FROM pg_collation WHERE oid = attcollation) AS coll FROM pg_attribute WHERE attrelid = 't2'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("coll", NAME)],
                        rows: &[
                            &[T("x"), T("integer"), Null],
                            &[T("y"), T("character varying(10)"), T("default")],
                            &[T("?column?"), T("text"), T("default")],
                            &[T("two"), T("integer"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", VARCHAR), Column("?column?", TEXT), Column("two", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("lit"), T("2")],
                            &[T("2"), T("y"), T("lit"), T("2")],
                            &[T("3"), Null, T("lit"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (x, y, z, w, v) AS SELECT a, b, c, d FROM src;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "too many column names were specified", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t4 AS SELECT a FROM src WITH NO DATA;",
                    expected: Expected::Tag("CREATE TABLE AS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t4;",
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
                    query: "CREATE TABLE t1 AS SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"t1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS t1 AS SELECT 1;",
                    expected: Expected::Tag("CREATE TABLE AS"),
                    notices: &[Diagnostic { code: "42P07", message: r#"relation "t1" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS t5 AS SELECT 7 AS seven;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t5;",
                    expected: Expected::Rows {
                        columns: &[Column("seven", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a * 10 AS a10, d INTO t6 FROM src WHERE a > 1;",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t6 ORDER BY a10;",
                    expected: Expected::Rows {
                        columns: &[Column("a10", INT4), Column("d", TEXT)],
                        rows: &[
                            &[T("20"), T("q")],
                            &[T("30"), T("r")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t7 AS SELECT * FROM src;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t7;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
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
                    query: "SELECT count(*) FROM pg_class WHERE relname = 't7';",
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
                    query: "CREATE TEMP TABLE t8 AS SELECT a FROM src;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 't8';",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNLOGGED TABLE t9 AS SELECT a FROM src;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t10 AS VALUES (1, 'one'), (2, 'two');",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t10 ORDER BY column1;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4), Column("column2", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t11 AS SELECT a, sum(c) FROM src GROUP BY a ORDER BY a;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t11;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("sum", NUMERIC)],
                        rows: &[
                            &[T("1"), T("1.50")],
                            &[T("2"), T("2.25")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t12 AS SELECT NULL AS n, ARRAY[1, 2] AS arr, ROW(1, 'x') AS r;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "column \"r\" has pseudo-type record", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t12 AS SELECT NULL AS n, ARRAY[1, 2] AS arr;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 't12'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT)],
                        rows: &[
                            &[T("n"), T("text")],
                            &[T("arr"), T("integer[]")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) CREATE TABLE t13 AS SELECT * FROM src WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on src")],
                            &[T("  Filter: (a = 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 't13';",
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
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TABLE t14 AS SELECT * FROM src WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on src (actual rows=1.00 loops=1)")],
                            &[T("  Filter: (a = 1)")],
                            &[T("  Rows Removed by Filter: 2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t14;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", VARCHAR), Column("c", NUMERIC), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("x"), T("1.50"), T("p")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t15 AS SELECT a FROM src WITH DATA;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t16 WITH (fillfactor = 50) AS SELECT a FROM src;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reloptions FROM pg_class WHERE relname = 't16';",
                    expected: Expected::Rows {
                        columns: &[Column("reloptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("{fillfactor=50}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t17 AS SELECT 1 AS a, 2 AS a;",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"a\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 AS q INTO t18 FROM src LIMIT 0;",
                    expected: Expected::Tag("SELECT 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t18;",
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
                    query: "CREATE TABLE nosuch.t19 AS SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: "schema \"nosuch\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_create_table_as_in_plpgsql() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TABLE AS in PL/pgSQL with a variable, EXECUTE with ROW_COUNT, and EXECUTE of SELECT ... INTO",
            set_up_script: &[
                "CREATE TABLE src (a int, b text);",
                "INSERT INTO src VALUES (1, 'x'), (2, 'y'), (3, 'z');",
                "CREATE FUNCTION mk(lim int) RETURNS bigint LANGUAGE plpgsql AS $$ DECLARE n bigint; BEGIN CREATE TABLE f1 AS SELECT * FROM src WHERE a <= lim; GET DIAGNOSTICS n = ROW_COUNT; RETURN n; END $$;",
                "CREATE FUNCTION mk2(t text) RETURNS bigint LANGUAGE plpgsql AS $$ DECLARE n bigint; BEGIN EXECUTE format('CREATE TABLE %I AS SELECT a FROM src', t); GET DIAGNOSTICS n = ROW_COUNT; RETURN n; END $$;",
                "CREATE FUNCTION mk3() RETURNS void LANGUAGE plpgsql AS $$ BEGIN EXECUTE 'SELECT 1 AS x INTO f3'; END $$;",
                "CREATE FUNCTION mk4() RETURNS bigint LANGUAGE plpgsql AS $$ DECLARE n bigint; BEGIN EXECUTE 'CREATE TABLE f4 AS SELECT 1 WITH NO DATA'; GET DIAGNOSTICS n = ROW_COUNT; RETURN n; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT mk(2);",
                    expected: Expected::Rows {
                        columns: &[Column("mk", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM f1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mk2('f2');",
                    expected: Expected::Rows {
                        columns: &[Column("mk2", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM f2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mk3();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "EXECUTE of SELECT ... INTO is not implemented", hint: "You might want to use EXECUTE ... INTO or EXECUTE CREATE TABLE ... AS instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mk4();",
                    expected: Expected::Rows {
                        columns: &[Column("mk4", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'f3';",
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
fn test_create_table_as_event_triggers() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TABLE AS and SELECT ... INTO in pg_event_trigger_ddl_commands",
            set_up_script: &[
                "CREATE TABLE evlog (tag text, otype text, ident text);",
                "CREATE FUNCTION evf() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_ddl_commands() LOOP INSERT INTO evlog VALUES (r.command_tag, r.object_type, r.object_identity); END LOOP; END $$;",
                "CREATE EVENT TRIGGER evt ON ddl_command_end EXECUTE FUNCTION evf();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE e1 AS SELECT 1 AS a;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 2 AS b INTO e2;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS e1 AS SELECT 1 AS a;",
                    notices: &[Diagnostic { code: "42P07", message: "relation \"e1\" already exists, skipping", ..N }],
                    expected: Expected::Tag("CREATE TABLE AS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE e3 AS SELECT 1 AS a WITH NO DATA;",
                    expected: Expected::Tag("CREATE TABLE AS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM evlog;",
                    expected: Expected::Rows {
                        columns: &[Column("tag", TEXT), Column("otype", TEXT), Column("ident", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE AS"), T("table"), T("public.e1")],
                            &[T("SELECT INTO"), T("table"), T("public.e2")],
                            &[T("CREATE TABLE AS"), T("table"), T("public.e3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_create_table_as_execute_and_explain_analyze() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TABLE AS EXECUTE and EXPLAIN ANALYZE CREATE TABLE AS",
            set_up_script: &[
                "CREATE TABLE src (a int, b text);",
                "INSERT INTO src VALUES (1, 'x'), (2, 'y'), (3, 'z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE p1(int) AS SELECT a, b FROM src WHERE a >= $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE x1 AS EXECUTE p1(2);",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM x1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE x2 (n, m) AS EXECUTE p1(1) WITH NO DATA;",
                    expected: Expected::Tag("SELECT 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM x2;",
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
                    query: "SELECT attname, format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'x2'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT)],
                        rows: &[
                            &[T("n"), T("integer")],
                            &[T("m"), T("text")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS x1 AS EXECUTE p1(1);",
                    notices: &[Diagnostic { code: "42P07", message: "relation \"x1\" already exists, skipping", ..N }],
                    expected: Expected::Tag("CREATE TABLE AS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p2 AS INSERT INTO src VALUES (4, 'w');",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE x3 AS EXECUTE p2;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "prepared statement is not a SELECT", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE x4 AS EXECUTE nosuch;",
                    expected: Expected::Error(Diagnostic { code: "26000", message: "prepared statement \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TABLE IF NOT EXISTS x1 AS SELECT 1;",
                    notices: &[Diagnostic { code: "42P07", message: "relation \"x1\" already exists, skipping", ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TABLE x5 AS SELECT a FROM src WITH NO DATA;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on src (never executed)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM x5;",
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
fn test_explain_create_table_as_execute() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN and EXPLAIN ANALYZE of CREATE TABLE AS EXECUTE",
            set_up_script: &[
                "CREATE TABLE src (a int, b text);",
                "INSERT INTO src VALUES (1, 'x'), (2, 'y'), (3, 'z');",
                "PREPARE p1(int) AS SELECT a FROM src WHERE a >= $1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) CREATE TABLE y0 AS EXECUTE p1(1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on src")],
                            &[T("  Filter: (a >= 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'y0';",
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
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TABLE y1 (n) AS EXECUTE p1(2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on src (actual rows=2.00 loops=1)")],
                            &[T("  Filter: (a >= 2)")],
                            &[T("  Rows Removed by Filter: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM y1 ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TABLE y2 AS EXECUTE p1(1) WITH NO DATA;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on src (never executed)")],
                            &[T("  Filter: (a >= 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM y2;",
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
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TABLE IF NOT EXISTS y1 AS EXECUTE p1(1);",
                    notices: &[Diagnostic { code: "42P07", message: "relation \"y1\" already exists, skipping", ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_cached_create_table_as_follows_its_query_tables() {
    run_scripts(&[
        ScriptTest {
            name: "a function's cached CREATE TABLE AS depends on the tables its query reads, so a column added to one plans it again",
            set_up_script: &[
                "CREATE TABLE qd_t (a int);",
                "INSERT INTO qd_t VALUES (1);",
                "CREATE FUNCTION qd_ctas() RETURNS void LANGUAGE plpgsql AS $$ BEGIN DROP TABLE IF EXISTS qd_x; CREATE TABLE qd_x AS SELECT * FROM qd_t; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT qd_ctas();",
                    expected: Expected::Rows {
                        columns: &[Column("qd_ctas", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "table \"qd_x\" does not exist, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE qd_t ADD COLUMN b int DEFAULT 7;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT qd_ctas();",
                    expected: Expected::Rows {
                        columns: &[Column("qd_ctas", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM qd_x;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("7")],
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
fn test_create_table_as_does_not_see_its_own_table() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TABLE AS reads the catalog as it was before the new table, as Postgres' snapshot does",
            set_up_script: &[
                "CREATE SCHEMA ctas_s;",
                "CREATE TABLE ctas_s.t1 (a INT);",
                "INSERT INTO ctas_s.t1 VALUES (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ctas_s.snap AS SELECT relname FROM pg_class WHERE relnamespace = 'ctas_s'::regnamespace;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM ctas_s.snap;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("t1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ctas_s.copy1 AS SELECT a * 10 AS b FROM ctas_s.t1;",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM ctas_s.copy1 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
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
fn test_create_table_as_does_not_check_insert_on_its_table() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TABLE AS and CREATE MATERIALIZED VIEW write their rows without the owner's INSERT privilege",
            set_up_script: &[
                "CREATE ROLE ctas_noinsert",
                "CREATE SCHEMA ctas_noinsert_s AUTHORIZATION ctas_noinsert",
                "ALTER DEFAULT PRIVILEGES FOR ROLE ctas_noinsert REVOKE INSERT ON TABLES FROM ctas_noinsert",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET ROLE ctas_noinsert;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ctas_noinsert_s.t (a) AS SELECT generate_series(1, 3) WITH DATA;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ctas_noinsert_s.t VALUES (4);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ctas_noinsert_s.t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW ctas_noinsert_s.mv (a) AS SELECT generate_series(1, 2) WITH DATA;",
                    expected: Expected::Tag("SELECT 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REFRESH MATERIALIZED VIEW ctas_noinsert_s.mv;",
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
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
fn test_create_temp_table_as_execute_makes_the_temporary_schema() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TEMP TABLE AS EXECUTE as a session's first temporary object",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE ctas_tmp_q AS SELECT 1 AS a;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE ctas_tmp_t AS EXECUTE ctas_tmp_q;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM ctas_tmp_t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE IF NOT EXISTS ctas_tmp_t AS EXECUTE ctas_tmp_q;",
                    expected: Expected::Tag("CREATE TABLE AS"),
                    notices: &[Diagnostic { code: "42P07", message: r#"relation "ctas_tmp_t" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE ctas_tmp_t AS EXECUTE ctas_tmp_q;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "ctas_tmp_t" already exists"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "EXPLAIN ANALYZE CREATE TEMP TABLE AS as a session's first temporary object",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TEMP TABLE ctas_tmp_e AS SELECT 1 AS a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Result (actual rows=1.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM ctas_tmp_e;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "EXPLAIN CREATE TEMP TABLE AS as a session's first temporary object",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) CREATE TEMP TABLE ctas_tmp_p AS SELECT 1 AS a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "EXPLAIN ANALYZE CREATE TEMP TABLE AS EXECUTE as a session's first temporary object",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE ctas_tmp_q2 AS SELECT 1 AS a;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TEMP TABLE ctas_tmp_x AS EXECUTE ctas_tmp_q2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Result (actual rows=1.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
