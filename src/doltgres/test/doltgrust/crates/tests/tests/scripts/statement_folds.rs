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
fn test_statement_folds() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER TABLE folds a default, a generation expression, and a CHECK constraint that call a PL/pgSQL function with constant arguments",
            set_up_script: &[
                "CREATE FUNCTION plus(i INT, j INT) RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN i + j; END $$;",
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 10), (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN d INT DEFAULT plus(1, 2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN g INT GENERATED ALWAYS AS (v + plus(1, 1)) STORED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD CONSTRAINT t_pos CHECK (v > plus(0, 0));",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("d", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("3"), T("12")],
                            &[T("2"), T("20"), T("3"), T("22")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index expressions and predicates that call a PL/pgSQL function with constant arguments pass CREATE INDEX's immutability checks",
            set_up_script: &[
                "CREATE FUNCTION plus(i INT, j INT) RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN i + j; END $$;",
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 10), (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX t_idx ON t (v) WHERE plus(2, 2) = 4;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_expr ON t ((v + plus(1, 1)));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE v + plus(1, 1) = 22;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE v = 10 AND plus(2, 2) = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
            name: "a partition key expression and a statistics object's expression that call a PL/pgSQL function with constant arguments",
            set_up_script: &[
                "CREATE FUNCTION plus(i INT, j INT) RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN i + j; END $$;",
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 10), (2, 20);",
                "CREATE TABLE p (a INT, b INT) PARTITION BY RANGE ((a + plus(1, 1)));",
                "CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (100);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("p1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS t_stats ON (v + plus(1, 1)), (id + 1) FROM t;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE t;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CALL and EXECUTE fold arguments that call a PL/pgSQL function with constant arguments",
            set_up_script: &[
                "CREATE FUNCTION plus(i INT, j INT) RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN i + j; END $$;",
                "CREATE TABLE log (i INT);",
                "CREATE PROCEDURE ins(i INT) LANGUAGE sql AS $$ INSERT INTO log VALUES (i) $$;",
                "PREPARE q(INT) AS SELECT $1 + 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CALL ins(plus(5, 5));",
                    expected: Expected::Tag("CALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM log;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE q(plus(1, 1));",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "partition bound values from a PL/pgSQL function are evaluated under the caller's TimeZone",
            set_up_script: &[
                "SET TIME ZONE 'America/New_York';",
                "CREATE FUNCTION tsf(t TEXT) RETURNS TIMESTAMPTZ STABLE LANGUAGE plpgsql AS $$ BEGIN RETURN t::timestamptz; END $$;",
                "CREATE TABLE tp (ts TIMESTAMPTZ) PARTITION BY RANGE (ts);",
                "CREATE TABLE tp2 (ts TIMESTAMPTZ);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE tp1 PARTITION OF tp FOR VALUES FROM (tsf('2020-01-01')) TO (tsf('2021-01-01'));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tp ATTACH PARTITION tp2 FOR VALUES FROM (tsf('2021-01-01')) TO (tsf('2022-01-01'));",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TIME ZONE 'UTC';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, pg_get_expr(relpartbound, oid) FROM pg_class WHERE relname IN ('tp1', 'tp2') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("pg_get_expr", TEXT)],
                        rows: &[
                            &[T("tp1"), T("FOR VALUES FROM ('2020-01-01 05:00:00+00') TO ('2021-01-01 05:00:00+00')")],
                            &[T("tp2"), T("FOR VALUES FROM ('2021-01-01 05:00:00+00') TO ('2022-01-01 05:00:00+00')")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a range bound value that a PL/pgSQL function evaluates to NULL is refused",
            set_up_script: &[
                "CREATE FUNCTION nul() RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN NULL; END $$;",
                "CREATE TABLE rp (a INT) PARTITION BY RANGE (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE rp1 PARTITION OF rp FOR VALUES FROM (nul()) TO (10);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "cannot specify NULL in range bound", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_constant_calls_the_planner_folds() {
    run_scripts(&[
        ScriptTest {
            name: "the planner folds calls of polymorphic, collation-sensitive, and length-coercing functions on constants as Postgres does, and raises their errors",
            set_up_script: &[
                "CREATE TABLE c (x CHAR(3));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT array_length(ARRAY[1, 2, 3], 1), array_append(ARRAY[1, 2], 3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  Output: 3, '{1,2,3}'::integer[]"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_length(ARRAY[1, 2, 3], 1), array_append(ARRAY[1, 2], 3);",
                    expected: Expected::Rows {
                        columns: &[Column("array_length", INT4), Column("array_append", INT4_ARRAY)],
                        rows: &[
                            &[T("3"), T("{1,2,3}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT 'a' < 'B' COLLATE \"C\", upper('abc'), 'x'::CHAR(3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  Output: false, 'ABC'::text, 'x  '::character(3)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sqrt(-1);",
                    expected: Expected::Error(Diagnostic { code: "2201F", message: "cannot take square root of a negative number", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 + 2147483647;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "integer out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES ('ab'), ('abcd');",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character(3)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES ('ab'), ('abc   ');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x, length(x) FROM c ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", BPCHAR), Column("length", INT4)],
                        rows: &[
                            &[T("ab "), T("2")],
                            &[T("abc"), T("3")],
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
