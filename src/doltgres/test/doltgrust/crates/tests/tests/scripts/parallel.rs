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
use harness::script::{A, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_parallel() {
    run_scripts(&[
        ScriptTest {
            name: "parallel query",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT);",
                "INSERT INTO t VALUES (1, 1);",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "CREATE TABLE d (id INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v TEXT);",
                "INSERT INTO d SELECT a, 'c' FROM t;",
                "CREATE TABLE k (a INT, b INT);",
                "INSERT INTO k SELECT 1, 1 FROM t;",
                "CREATE TABLE w (x INT);",
                "CREATE FUNCTION f_sql(x INT) RETURNS INT LANGUAGE sql PARALLEL SAFE AS 'SELECT x * 2';",
                "CREATE FUNCTION f_pl(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN RETURN x + 1; END $$;",
                "CREATE FUNCTION f_restricted(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL RESTRICTED AS $$ BEGIN RETURN x + 1; END $$;",
                "CREATE FUNCTION f_default(x INT) RETURNS INT LANGUAGE plpgsql AS $$ BEGIN RETURN x + 1; END $$;",
                "CREATE FUNCTION f_writes(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN INSERT INTO w VALUES (x); RETURN x; END $$;",
                "CREATE FUNCTION f_sql_writes(x INT) RETURNS INT LANGUAGE sql PARALLEL SAFE AS 'INSERT INTO w VALUES (x) RETURNING x';",
                "CREATE FUNCTION f_notice(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN IF x = 5 THEN RAISE NOTICE 'five %', x; END IF; RETURN x; END $$;",
                "CREATE FUNCTION f_error(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN IF x = 7000 THEN RAISE EXCEPTION 'boom %', x; END IF; RETURN x; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE t, d, k;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_setup_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_tuple_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET min_parallel_table_scan_size = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 4;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_leader_participation = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM t WHERE a < 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather"#)],
                            &[T(r#"  Workers Planned: 4"#)],
                            &[T(r#"  ->  Parallel Seq Scan on t"#)],
                            &[T(r#"        Filter: (a < 5)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t WHERE a < 5 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(a) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Seq Scan on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16384"), T("134225920")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, count(*), sum(a) FROM t GROUP BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: b"#)],
                            &[T(r#"  ->  Gather Merge"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Sort Key: b"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: b"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(*), sum(a) FROM t GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), T("1638"), T("13423410")],
                            &[T("1"), T("1639"), T("13425049")],
                            &[T("2"), T("1639"), T("13426688")],
                            &[T("3"), T("1639"), T("13428327")],
                            &[T("4"), T("1639"), T("13429966")],
                            &[T("5"), T("1638"), T("13415220")],
                            &[T("6"), T("1638"), T("13416858")],
                            &[T("7"), T("1638"), T("13418496")],
                            &[T("8"), T("1638"), T("13420134")],
                            &[T("9"), T("1638"), T("13421772")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT DISTINCT b FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Unique"#)],
                            &[T(r#"  ->  Gather Merge"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Sort Key: b"#)],
                            &[T(r#"              ->  HashAggregate"#)],
                            &[T(r#"                    Group Key: b"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT b FROM t ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT a, b FROM t WHERE b = 1) s WHERE a < 30;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather"#)],
                            &[T(r#"  Workers Planned: 4"#)],
                            &[T(r#"  ->  Parallel Seq Scan on t"#)],
                            &[T(r#"        Filter: ((a < 30) AND (b = 1))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a, b FROM t WHERE b = 1) s WHERE a < 30 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("11"), T("1")],
                            &[T("21"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_leader_participation = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16384"), T("134225920")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM t WHERE b = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  ->  Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        Workers Launched: 0"#)],
                            &[T(r#"        ->  Partial Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"              ->  Parallel Seq Scan on t (actual rows=1639.00 loops=1)"#)],
                            &[T(r#"                    Filter: (b = 3)"#)],
                            &[T(r#"                    Rows Removed by Filter: 14745"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE b = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1639")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET max_parallel_workers;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_leader_participation = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d SELECT id, 'dup' FROM d WHERE id % 100 = 0;",
                    expected: Expected::Tag("INSERT 0 163"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(id) FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Seq Scan on d"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16547"), T("135562520")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v, count(*) FROM d GROUP BY v ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("c"), T("16384")],
                            &[T("dup"), T("163")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k SELECT 1, 1 FROM t WHERE a <= 777;",
                    expected: Expected::Tag("INSERT 0 777"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM k;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Seq Scan on k"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("Doltgrust counts a keyless table's identical rows as one entry of a tree of the rows it sees, so this table of 17,161 rows of (1, 1) takes one page where Postgres' takes dozens (73 for 16,384 of them), too few for parallel workers"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM k;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("17161"), T("17161")],
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
                    query: "SELECT proname, proparallel FROM pg_proc WHERE proname IN ('f_sql', 'f_pl', 'f_restricted', 'f_default') ORDER BY proname;",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("proparallel", CHAR)],
                        rows: &[
                            &[T("f_default"), T("u")],
                            &[T("f_pl"), T("s")],
                            &[T("f_restricted"), T("r")],
                            &[T("f_sql"), T("s")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT sum(f_sql(a)), sum(f_pl(a)) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Seq Scan on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(f_sql(a)), sum(f_pl(a)) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("268451840"), T("134242304")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT sum(f_restricted(a)) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Parallel Seq Scan on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT sum(f_default(a)) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate"#)],
                            &[T(r#"  ->  Seq Scan on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_writes(a) FROM t WHERE a < 3;",
                    expected: Expected::Error(Diagnostic { code: "25000", message: "cannot execute INSERT during a parallel operation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_sql_writes(a) FROM t WHERE a < 3;",
                    expected: Expected::Error(Diagnostic { code: "25000", message: "cannot execute INSERT during a parallel operation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM w;",
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
                    query: "SELECT f_notice(a) FROM t WHERE a = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("f_notice", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "five 5", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(f_error(a)) FROM t;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "boom 7000", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "parallel query refuses a write in a worker that debug_parallel_query starts",
            set_up_script: &[
                "CREATE TABLE w (x INT);",
                "CREATE FUNCTION f_writes(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN INSERT INTO w VALUES (x); RETURN x; END $$;",
                "CREATE FUNCTION f_sql_writes(x INT) RETURNS INT LANGUAGE sql PARALLEL SAFE AS 'INSERT INTO w VALUES (x) RETURNING x';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET debug_parallel_query = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT f_writes(1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Result"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_writes(1);",
                    expected: Expected::Error(Diagnostic { code: "25000", message: "cannot execute INSERT during a parallel operation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_sql_writes(1);",
                    expected: Expected::Error(Diagnostic { code: "25000", message: "cannot execute INSERT during a parallel operation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET debug_parallel_query;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f_writes(1);",
                    expected: Expected::Rows {
                        columns: &[Column("f_writes", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "parallel workers' scans reach the table statistics",
            set_up_script: &[
                "CREATE TABLE ps (a INT);",
                "INSERT INTO ps VALUES (1), (2), (3), (4), (5), (6), (7), (8);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET parallel_setup_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_tuple_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET min_parallel_table_scan_size = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_leader_participation = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ps;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT seq_scan, seq_tup_read FROM pg_stat_user_tables WHERE relname = 'ps';",
                    expected: Expected::Rows {
                        columns: &[Column("seq_scan", INT8), Column("seq_tup_read", INT8)],
                        rows: &[
                            &[T("3"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ps;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_stat_force_next_flush();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_stat_force_next_flush", VOID)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT seq_scan, seq_tup_read FROM pg_stat_user_tables WHERE relname = 'ps';",
                    expected: Expected::Rows {
                        columns: &[Column("seq_scan", INT8), Column("seq_tup_read", INT8)],
                        rows: &[
                            &[T("4"), T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("pg_stat_user_tables and pg_stat_force_next_flush come with the pg_stat functions"),
            ..S
        },
        ScriptTest {
            name: "parallel query's worker waits for a lock another session holds on a table its function reads",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT)",
                "INSERT INTO t VALUES (1, 1)",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "CREATE TABLE t2 (x INT)",
                "INSERT INTO t2 VALUES (1)",
                "CREATE FUNCTION f_reads_t2(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN RETURN x + (SELECT count(*) FROM t2); END $$",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ LOCK TABLE t2 IN ACCESS EXCLUSIVE MODE",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_setup_cost = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_tuple_cost = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET min_parallel_table_scan_size = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_leader_participation = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(f_reads_t2(a)) FROM t",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM t2",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "parallel query's leader stops a worker that waits for a lock when the statement times out",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT)",
                "INSERT INTO t VALUES (1, 1)",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "CREATE TABLE t2 (x INT)",
                "INSERT INTO t2 VALUES (1)",
                "CREATE FUNCTION f_reads_t2(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN RETURN x + (SELECT count(*) FROM t2); END $$",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ LOCK TABLE t2 IN ACCESS EXCLUSIVE MODE",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_setup_cost = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_tuple_cost = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET min_parallel_table_scan_size = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_leader_participation = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET statement_timeout = 500",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(f_reads_t2(a)) FROM t",
                    expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to statement timeout", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ RESET statement_timeout",
                    expected: Expected::Tag("RESET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(f_reads_t2(a)) FROM t",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1024")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_parallel_merge_append_and_index_scans() {
    run_scripts(&[
        ScriptTest {
            name: "parallel query (b)",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT);",
                "INSERT INTO t VALUES (1, 1);",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "CREATE INDEX t_a ON t (a);",
                "CREATE INDEX t_b ON t (b);",
                "CREATE TABLE d (id INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v TEXT);",
                "INSERT INTO d SELECT a, 'c' FROM t;",
                "CREATE TABLE k (a INT, b INT);",
                "INSERT INTO k SELECT a % 50, 1 FROM t;",
                "CREATE INDEX k_a ON k (a);",
                "CREATE TABLE p (a INT, b INT) PARTITION BY RANGE (a);",
                "CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (5000);",
                "CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (5000) TO (10000);",
                "CREATE TABLE p3 PARTITION OF p FOR VALUES FROM (10000) TO (20000);",
                "INSERT INTO p SELECT a, b FROM t;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "VACUUM ANALYZE t, d, k, p;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_setup_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_tuple_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET min_parallel_table_scan_size = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET min_parallel_index_scan_size = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 4;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_leader_participation = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM t WHERE a % 1000 = 0 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather Merge"#)],
                            &[T(r#"  Workers Planned: 4"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: a"#)],
                            &[T(r#"        ->  Parallel Seq Scan on t"#)],
                            &[T(r#"              Filter: ((a % 1000) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE a % 1000 = 0 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1000")],
                            &[T("2000")],
                            &[T("3000")],
                            &[T("4000")],
                            &[T("5000")],
                            &[T("6000")],
                            &[T("7000")],
                            &[T("8000")],
                            &[T("9000")],
                            &[T("10000")],
                            &[T("11000")],
                            &[T("12000")],
                            &[T("13000")],
                            &[T("14000")],
                            &[T("15000")],
                            &[T("16000")],
                        ],
                        tag: "SELECT 16",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b FROM t WHERE a % 500 = 0 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather Merge"#)],
                            &[T(r#"  Workers Planned: 4"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: b"#)],
                            &[T(r#"        ->  Parallel Seq Scan on t"#)],
                            &[T(r#"              Filter: ((a % 500) = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM t WHERE a % 500 = 0 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                            &[T("0")],
                        ],
                        tag: "SELECT 32",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashagg = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, count(*), sum(a) FROM t GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: b"#)],
                            &[T(r#"  ->  Gather Merge"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial GroupAggregate"#)],
                            &[T(r#"              Group Key: b"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: b"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(*), sum(a) FROM t GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), T("1638"), T("13423410")],
                            &[T("1"), T("1639"), T("13425049")],
                            &[T("2"), T("1639"), T("13426688")],
                            &[T("3"), T("1639"), T("13428327")],
                            &[T("4"), T("1639"), T("13429966")],
                            &[T("5"), T("1638"), T("13415220")],
                            &[T("6"), T("1638"), T("13416858")],
                            &[T("7"), T("1638"), T("13418496")],
                            &[T("8"), T("1638"), T("13420134")],
                            &[T("9"), T("1638"), T("13421772")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashagg;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(a) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Append"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on p3 p_3"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on p2 p_2"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on p1 p_1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16384"), T("134225920")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_parallel_append = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(a) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on p1 p_1"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on p2 p_2"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on p3 p_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16384"), T("134225920")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_parallel_append;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(a) FROM t WHERE a > 100;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Index Only Scan using t_a on t"#)],
                            &[T(r#"                    Index Cond: (a > 100)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM t WHERE a > 100;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16284"), T("134220870")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM t WHERE b = 3 AND a < 100 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using t_a on t"#)],
                            &[T(r#"  Index Cond: (a < 100)"#)],
                            &[T(r#"  Filter: (b = 3)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM t WHERE b = 3 AND a < 100 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("13"), T("3")],
                            &[T("23"), T("3")],
                            &[T("33"), T("3")],
                            &[T("43"), T("3")],
                            &[T("53"), T("3")],
                            &[T("63"), T("3")],
                            &[T("73"), T("3")],
                            &[T("83"), T("3")],
                            &[T("93"), T("3")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d SELECT id, 'dup' FROM d WHERE id % 100 = 0;",
                    expected: Expected::Tag("INSERT 0 163"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(id) FROM d WHERE id > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Index Only Scan using d_pkey on d"#)],
                            &[T(r#"                    Index Cond: (id > 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM d WHERE id > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16547"), T("135562520")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v, count(*) FROM d WHERE id > 0 GROUP BY v ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("c"), T("16384")],
                            &[T("dup"), T("163")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k SELECT a % 50, 1 FROM t WHERE a <= 777;",
                    expected: Expected::Tag("INSERT 0 777"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(a) FROM k WHERE a >= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 3"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Index Only Scan using k_a on k"#)],
                            &[T(r#"                    Index Cond: (a >= 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("a table without a primary key keeps one index entry for each distinct row with its count, so its index has fewer pages than Postgres and gets fewer workers"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM k WHERE a >= 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("17161"), T("419923")],
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
                    query: "SET enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexonlyscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(a) FROM t WHERE b < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Bitmap Heap Scan on t"#)],
                            &[T(r#"                    Recheck Cond: (b < 3)"#)],
                            &[T(r#"                    ->  Bitmap Index Scan on t_b"#)],
                            &[T(r#"                          Index Cond: (b < 3)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM t WHERE b < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("4916"), T("40275147")],
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
fn test_parallel_hash_join() {
    run_scripts(&[
        ScriptTest {
            name: "parallel hash join",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT);",
                "INSERT INTO t VALUES (1, 1);",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "CREATE TABLE s (a INT, c INT);",
                "INSERT INTO s SELECT a, a % 7 FROM t WHERE a % 3 = 1;",
                "CREATE TABLE e (a INT);",
                "CREATE TABLE big (a INT, pad TEXT);",
                "INSERT INTO big SELECT a % 5000, repeat('x', 50) FROM t;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "VACUUM ANALYZE t, s, e, big;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_setup_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_tuple_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET min_parallel_table_scan_size = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_leader_participation = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_mergejoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_nestloop = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(t.a), sum(s.c) FROM t JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                            &[T(r#"                    ->  Parallel Hash"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(t.a), sum(s.c) FROM t JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("5462"), T("44747435"), T("16385")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(s.c) FROM t LEFT JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Hash Left Join"#)],
                            &[T(r#"                    Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                            &[T(r#"                    ->  Parallel Hash"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(s.c) FROM t LEFT JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16384"), T("16385")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(t.a), sum(s.a) FROM t RIGHT JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Hash Right Join"#)],
                            &[T(r#"                    Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                            &[T(r#"                    ->  Parallel Hash"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(t.a), sum(s.a) FROM t RIGHT JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("5462"), T("44747435"), T("44747435")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(t.a), sum(s.a) FROM t FULL JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Hash Full Join"#)],
                            &[T(r#"                    Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                            &[T(r#"                    ->  Parallel Hash"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(t.a), sum(s.a) FROM t FULL JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16384"), T("134225920"), T("44747435")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM t WHERE NOT EXISTS (SELECT 1 FROM s WHERE s.a = t.a);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Hash Anti Join"#)],
                            &[T(r#"                    Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                            &[T(r#"                    ->  Parallel Hash"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE NOT EXISTS (SELECT 1 FROM s WHERE s.a = t.a);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10922")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM s WHERE EXISTS (SELECT 1 FROM t WHERE s.a = t.a);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Hash Semi Join"#)],
                            &[T(r#"                    Hash Cond: (s.a = t.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on s"#)],
                            &[T(r#"                    ->  Parallel Hash"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM s WHERE EXISTS (SELECT 1 FROM t WHERE s.a = t.a);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5462")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM t JOIN e ON t.a = e.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Parallel Hash Join"#)],
                            &[T(r#"              Hash Cond: (t.a = e.a)"#)],
                            &[T(r#"              ->  Parallel Seq Scan on t"#)],
                            &[T(r#"              ->  Parallel Hash"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on e"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t JOIN e ON t.a = e.a;",
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
                    query: "SELECT count(*) FROM t LEFT JOIN e ON t.a = e.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16384")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(t.a), sum(s.c) FROM t JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                            &[T(r#"                    ->  Parallel Hash"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(t.a), sum(s.c) FROM t JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("5462"), T("44747435"), T("16385")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(t.a), sum(s.a) FROM t FULL JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16384"), T("134225920"), T("44747435")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE NOT EXISTS (SELECT 1 FROM s WHERE s.a = t.a);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10922")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(length(big.pad)) FROM t JOIN big ON t.a = big.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16381"), T("819050")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_leader_participation = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM t JOIN s ON t.a = s.a;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  ->  Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        Workers Launched: 0"#)],
                            &[T(r#"        ->  Partial Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"              ->  Parallel Hash Join (actual rows=5462.00 loops=1)"#)],
                            &[T(r#"                    Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t (actual rows=16384.00 loops=1)"#)],
                            &[T(r#"                    ->  Parallel Hash (actual rows=5462.00 loops=1)"#)],
                            &[T(r#"                          Buckets: 8192  Batches: 1  Memory Usage: 288kB"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s (actual rows=5462.00 loops=1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM t FULL JOIN big ON t.a = big.a;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  ->  Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        Workers Launched: 0"#)],
                            &[T(r#"        ->  Partial Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"              ->  Parallel Hash Full Join (actual rows=27769.00 loops=1)"#)],
                            &[T(r#"                    Hash Cond: (big.a = t.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on big (actual rows=16384.00 loops=1)"#)],
                            &[T(r#"                    ->  Parallel Hash (actual rows=16384.00 loops=1)"#)],
                            &[T(r#"                          Buckets: 4096  Batches: 8  Memory Usage: 128kB"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on t (actual rows=16384.00 loops=1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET max_parallel_workers;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(t.a), sum(s.c) FROM t JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("5462"), T("44747435"), T("16385")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(t.a), sum(s.a) FROM t FULL JOIN s ON t.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("16384"), T("134225920"), T("44747435")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x, (SELECT count(*) FROM t JOIN s ON t.a = s.a AND s.c = x) FROM (VALUES (0), (1), (2), (3)) v(x);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("780")],
                            &[T("1"), T("781")],
                            &[T("2"), T("780")],
                            &[T("3"), T("780")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "parallel hash join's leader stops a worker that waits for a lock while the others wait to build the table",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT)",
                "INSERT INTO t VALUES (1, 1)",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "CREATE TABLE s (a INT, c INT)",
                "INSERT INTO s SELECT a, a % 7 FROM t WHERE a % 3 = 1",
                "CREATE TABLE t2 (x INT)",
                "INSERT INTO t2 VALUES (1)",
                "CREATE FUNCTION f_reads_t2(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN RETURN x + (SELECT count(*) FROM t2); END $$",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ VACUUM ANALYZE t, s",
                    expected: Expected::Tag("VACUUM"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ LOCK TABLE t2 IN ACCESS EXCLUSIVE MODE",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_setup_cost = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_tuple_cost = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET min_parallel_table_scan_size = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET max_parallel_workers_per_gather = 2",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_leader_participation = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_mergejoin = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET enable_nestloop = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ EXPLAIN (COSTS OFF) SELECT count(*) FROM t JOIN (SELECT a FROM s WHERE a <> 1 OR f_reads_t2(a) IS NOT NULL) s2 ON t.a = s2.a",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t"#)],
                            &[T(r#"                    ->  Parallel Hash"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s"#)],
                            &[T(r#"                                Filter: ((a <> 1) OR (f_reads_t2(a) IS NOT NULL))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET statement_timeout = 500",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM t JOIN (SELECT a FROM s WHERE a <> 1 OR f_reads_t2(a) IS NOT NULL) s2 ON t.a = s2.a",
                    expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to statement timeout", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ RESET statement_timeout",
                    expected: Expected::Tag("RESET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM t JOIN (SELECT a FROM s WHERE a <> 1 OR f_reads_t2(a) IS NOT NULL) s2 ON t.a = s2.a",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5462")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "parallel hash join growing its batches from one",
            set_up_script: &[
                "CREATE TABLE t2 AS SELECT a FROM generate_series(1, 100000) a;",
                "CREATE TABLE s3 (a INT, c INT) WITH (autovacuum_enabled = false);",
                "INSERT INTO s3 SELECT a, a % 7 FROM generate_series(1, 100) a;",
                "ANALYZE s3;",
                "INSERT INTO s3 SELECT a, a % 7 FROM generate_series(101, 100000) a;",
                "VACUUM t2;",
                r#"CREATE FUNCTION explain_hash(q TEXT) RETURNS SETOF TEXT LANGUAGE plpgsql AS $$ DECLARE r TEXT; BEGIN FOR r IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP IF r NOT LIKE '%Buckets:%' THEN RETURN NEXT r; END IF; END LOOP; END $$;"#,
                "SET parallel_setup_cost = 0;",
                "SET parallel_tuple_cost = 0;",
                "SET min_parallel_table_scan_size = 0;",
                "SET max_parallel_workers_per_gather = 2;",
                "SET parallel_leader_participation = off;",
                "SET enable_mergejoin = off;",
                "SET enable_nestloop = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET work_mem = '1MB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM explain_hash('SELECT count(*) FROM t2 JOIN s3 ON t2.a = s3.a');",
                    expected: Expected::Rows {
                        columns: &[Column("explain_hash", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  ->  Gather (actual rows=2.00 loops=1)"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        Workers Launched: 2"#)],
                            &[T(r#"        ->  Partial Aggregate (actual rows=1.00 loops=2)"#)],
                            &[T(r#"              ->  Parallel Hash Join (actual rows=50000.00 loops=2)"#)],
                            &[T(r#"                    Hash Cond: (t2.a = s3.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t2 (actual rows=50000.00 loops=2)"#)],
                            &[T(r#"                    ->  Parallel Hash (actual rows=50000.00 loops=2)"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s3 (actual rows=50000.00 loops=2)"#)],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(s3.c) FROM t2 JOIN s3 ON t2.a = s3.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("100000"), T("300000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(s3.c) FROM t2 JOIN s3 ON t2.a = s3.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("100000"), T("300000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM explain_hash('SELECT count(*) FROM t2 JOIN s3 ON t2.a = s3.a');",
                    expected: Expected::Rows {
                        columns: &[Column("explain_hash", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  ->  Gather (actual rows=2.00 loops=1)"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        Workers Launched: 2"#)],
                            &[T(r#"        ->  Partial Aggregate (actual rows=1.00 loops=2)"#)],
                            &[T(r#"              ->  Parallel Hash Join (actual rows=50000.00 loops=2)"#)],
                            &[T(r#"                    Hash Cond: (t2.a = s3.a)"#)],
                            &[T(r#"                    ->  Parallel Seq Scan on t2 (actual rows=50000.00 loops=2)"#)],
                            &[T(r#"                    ->  Parallel Hash (actual rows=50000.00 loops=2)"#)],
                            &[T(r#"                          ->  Parallel Seq Scan on s3 (actual rows=50000.00 loops=2)"#)],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_parallel_instrumentation() {
    run_scripts(&[
        ScriptTest {
            name: "parallel query's workers report their plan nodes to EXPLAIN ANALYZE under debug_parallel_query",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT);",
                "INSERT INTO t VALUES (1, 1);",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x;",
                "CREATE TABLE s (a INT, c INT);",
                "INSERT INTO s SELECT a, a % 7 FROM t WHERE a % 3 = 1;",
                "CREATE TABLE e (a INT);",
                "CREATE TABLE big (a INT, pad TEXT);",
                "INSERT INTO big SELECT a % 5000, repeat('x', 50) FROM t;",
                "CREATE INDEX t_a ON t (a);",
                "CREATE TABLE k (a INT, b INT);",
                "INSERT INTO k SELECT a % 50, 1 FROM t;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "VACUUM ANALYZE t, s, e, big, k;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET debug_parallel_query = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM t WHERE b < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Output: (count(*))"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Workers Launched: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Output: count(*)"#)],
                            &[T(r#"        Worker 0:  actual rows=1.00 loops=1"#)],
                            &[T(r#"        ->  Seq Scan on public.t (actual rows=4916.00 loops=1)"#)],
                            &[T(r#"              Output: a, b"#)],
                            &[T(r#"              Filter: (t.b < 3)"#)],
                            &[T(r#"              Rows Removed by Filter: 11468"#)],
                            &[T(r#"              Worker 0:  actual rows=4916.00 loops=1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM t WHERE b < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Workers Launched: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        ->  Seq Scan on t (actual rows=4916.00 loops=1)"#)],
                            &[T(r#"              Filter: (b < 3)"#)],
                            &[T(r#"              Rows Removed by Filter: 11468"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM t WHERE b < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", JSON)],
                        rows: &[&[T("[\n  {\n    \"Plan\": {\n      \"Node Type\": \"Gather\",\n      \"Parallel Aware\": false,\n      \"Async Capable\": false,\n      \"Actual Rows\": 1.00,\n      \"Actual Loops\": 1,\n      \"Disabled\": false,\n      \"Output\": [\"(count(*))\"],\n      \"Workers Planned\": 1,\n      \"Workers Launched\": 1,\n      \"Single Copy\": true,\n      \"Plans\": [\n        {\n          \"Node Type\": \"Aggregate\",\n          \"Strategy\": \"Plain\",\n          \"Partial Mode\": \"Simple\",\n          \"Parent Relationship\": \"Outer\",\n          \"Parallel Aware\": false,\n          \"Async Capable\": false,\n          \"Actual Rows\": 1.00,\n          \"Actual Loops\": 1,\n          \"Disabled\": false,\n          \"Output\": [\"count(*)\"],\n          \"Workers\": [\n            {\n              \"Worker Number\": 0,\n              \"Actual Rows\": 1.00,\n              \"Actual Loops\": 1\n            }\n          ],\n          \"Plans\": [\n            {\n              \"Node Type\": \"Seq Scan\",\n              \"Parent Relationship\": \"Outer\",\n              \"Parallel Aware\": false,\n              \"Async Capable\": false,\n              \"Relation Name\": \"t\",\n              \"Schema\": \"public\",\n              \"Alias\": \"t\",\n              \"Actual Rows\": 4916.00,\n              \"Actual Loops\": 1,\n              \"Disabled\": false,\n              \"Output\": [\"a\", \"b\"],\n              \"Filter\": \"(t.b < 3)\",\n              \"Rows Removed by Filter\": 11468,\n              \"Workers\": [\n                {\n                  \"Worker Number\": 0,\n                  \"Actual Rows\": 4916.00,\n                  \"Actual Loops\": 1\n                }\n              ]\n            }\n          ]\n        }\n      ]\n    },\n    \"Triggers\": [\n    ]\n  }\n]")]],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT YAML) SELECT count(*) FROM t WHERE b < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[&[T("- Plan: \n    Node Type: \"Gather\"\n    Parallel Aware: false\n    Async Capable: false\n    Actual Rows: 1.00\n    Actual Loops: 1\n    Disabled: false\n    Output: \n      - \"(count(*))\"\n    Workers Planned: 1\n    Workers Launched: 1\n    Single Copy: true\n    Plans: \n      - Node Type: \"Aggregate\"\n        Strategy: \"Plain\"\n        Partial Mode: \"Simple\"\n        Parent Relationship: \"Outer\"\n        Parallel Aware: false\n        Async Capable: false\n        Actual Rows: 1.00\n        Actual Loops: 1\n        Disabled: false\n        Output: \n          - \"count(*)\"\n        Workers: \n          - Worker Number: 0\n            Actual Rows: 1.00\n            Actual Loops: 1\n        Plans: \n          - Node Type: \"Seq Scan\"\n            Parent Relationship: \"Outer\"\n            Parallel Aware: false\n            Async Capable: false\n            Relation Name: \"t\"\n            Schema: \"public\"\n            Alias: \"t\"\n            Actual Rows: 4916.00\n            Actual Loops: 1\n            Disabled: false\n            Output: \n              - \"a\"\n              - \"b\"\n            Filter: \"(t.b < 3)\"\n            Rows Removed by Filter: 11468\n            Workers: \n              - Worker Number: 0\n                Actual Rows: 4916.00\n                Actual Loops: 1\n  Triggers: ")]],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT XML) SELECT count(*) FROM t WHERE b < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", XML)],
                        rows: &[&[T("<explain xmlns=\"http://www.postgresql.org/2009/explain\">\n  <Query>\n    <Plan>\n      <Node-Type>Gather</Node-Type>\n      <Parallel-Aware>false</Parallel-Aware>\n      <Async-Capable>false</Async-Capable>\n      <Actual-Rows>1.00</Actual-Rows>\n      <Actual-Loops>1</Actual-Loops>\n      <Disabled>false</Disabled>\n      <Output>\n        <Item>(count(*))</Item>\n      </Output>\n      <Workers-Planned>1</Workers-Planned>\n      <Workers-Launched>1</Workers-Launched>\n      <Single-Copy>true</Single-Copy>\n      <Plans>\n        <Plan>\n          <Node-Type>Aggregate</Node-Type>\n          <Strategy>Plain</Strategy>\n          <Partial-Mode>Simple</Partial-Mode>\n          <Parent-Relationship>Outer</Parent-Relationship>\n          <Parallel-Aware>false</Parallel-Aware>\n          <Async-Capable>false</Async-Capable>\n          <Actual-Rows>1.00</Actual-Rows>\n          <Actual-Loops>1</Actual-Loops>\n          <Disabled>false</Disabled>\n          <Output>\n            <Item>count(*)</Item>\n          </Output>\n          <Workers>\n            <Worker>\n              <Worker-Number>0</Worker-Number>\n              <Actual-Rows>1.00</Actual-Rows>\n              <Actual-Loops>1</Actual-Loops>\n            </Worker>\n          </Workers>\n          <Plans>\n            <Plan>\n              <Node-Type>Seq Scan</Node-Type>\n              <Parent-Relationship>Outer</Parent-Relationship>\n              <Parallel-Aware>false</Parallel-Aware>\n              <Async-Capable>false</Async-Capable>\n              <Relation-Name>t</Relation-Name>\n              <Schema>public</Schema>\n              <Alias>t</Alias>\n              <Actual-Rows>4916.00</Actual-Rows>\n              <Actual-Loops>1</Actual-Loops>\n              <Disabled>false</Disabled>\n              <Output>\n                <Item>a</Item>\n                <Item>b</Item>\n              </Output>\n              <Filter>(t.b &lt; 3)</Filter>\n              <Rows-Removed-by-Filter>11468</Rows-Removed-by-Filter>\n              <Workers>\n                <Worker>\n                  <Worker-Number>0</Worker-Number>\n                  <Actual-Rows>4916.00</Actual-Rows>\n                  <Actual-Loops>1</Actual-Loops>\n                </Worker>\n              </Workers>\n            </Plan>\n          </Plans>\n        </Plan>\n      </Plans>\n    </Plan>\n    <Triggers>\n    </Triggers>\n  </Query>\n</explain>")]],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*), sum(s.c) FROM t JOIN s ON t.a = s.a;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Output: (count(*)), (sum(s.c))"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Workers Launched: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Output: count(*), sum(s.c)"#)],
                            &[T(r#"        Worker 0:  actual rows=1.00 loops=1"#)],
                            &[T(r#"        ->  Hash Join (actual rows=5462.00 loops=1)"#)],
                            &[T(r#"              Output: s.c"#)],
                            &[T(r#"              Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"              Worker 0:  actual rows=5462.00 loops=1"#)],
                            &[T(r#"              ->  Seq Scan on public.t (actual rows=16384.00 loops=1)"#)],
                            &[T(r#"                    Output: t.a, t.b"#)],
                            &[T(r#"                    Worker 0:  actual rows=16384.00 loops=1"#)],
                            &[T(r#"              ->  Hash (actual rows=5462.00 loops=1)"#)],
                            &[T(r#"                    Output: s.c, s.a"#)],
                            &[T(r#"                    Buckets: 8192  Batches: 1  Memory Usage: 278kB"#)],
                            &[T(r#"                    Worker 0:  actual rows=5462.00 loops=1"#)],
                            &[T(r#"                    ->  Seq Scan on public.s (actual rows=5462.00 loops=1)"#)],
                            &[T(r#"                          Output: s.c, s.a"#)],
                            &[T(r#"                          Worker 0:  actual rows=5462.00 loops=1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*), sum(s.c) FROM t JOIN s ON t.a = s.a;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Workers Launched: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        ->  Hash Join (actual rows=5462.00 loops=1)"#)],
                            &[T(r#"              Hash Cond: (t.a = s.a)"#)],
                            &[T(r#"              ->  Seq Scan on t (actual rows=16384.00 loops=1)"#)],
                            &[T(r#"              ->  Hash (actual rows=5462.00 loops=1)"#)],
                            &[T(r#"                    Buckets: 8192  Batches: 1  Memory Usage: 278kB"#)],
                            &[T(r#"                    ->  Seq Scan on s (actual rows=5462.00 loops=1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT a FROM t WHERE a = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Output: a"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Workers Launched: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Index Only Scan using t_a on public.t (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Output: a"#)],
                            &[T(r#"        Index Cond: (t.a = 7)"#)],
                            &[T(r#"        Heap Fetches: 0"#)],
                            &[T(r#"        Index Searches: 1"#)],
                            &[T(r#"        Worker 0:  actual rows=1.00 loops=1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashjoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_mergejoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM k JOIN t ON t.a = k.a;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Output: (count(*))"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Workers Launched: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Output: count(*)"#)],
                            &[T(r#"        Worker 0:  actual rows=1.00 loops=1"#)],
                            &[T(r#"        ->  Nested Loop (actual rows=16057.00 loops=1)"#)],
                            &[T(r#"              Worker 0:  actual rows=16057.00 loops=1"#)],
                            &[T(r#"              ->  Seq Scan on public.k (actual rows=16384.00 loops=1)"#)],
                            &[T(r#"                    Output: k.a, k.b"#)],
                            &[T(r#"                    Worker 0:  actual rows=16384.00 loops=1"#)],
                            &[T(r#"              ->  Memoize (actual rows=0.98 loops=16384)"#)],
                            &[T(r#"                    Output: t.a"#)],
                            &[T(r#"                    Cache Key: k.a"#)],
                            &[T(r#"                    Cache Mode: logical"#)],
                            &[T(r#"                    Worker 0:  actual rows=0.98 loops=16384"#)],
                            &[T(r#"                      Hits: 16334  Misses: 50  Evictions: 0  Overflows: 0  Memory Usage: 6kB"#)],
                            &[T(r#"                    ->  Index Only Scan using t_a on public.t (actual rows=0.98 loops=50)"#)],
                            &[T(r#"                          Output: t.a"#)],
                            &[T(r#"                          Index Cond: (t.a = k.a)"#)],
                            &[T(r#"                          Heap Fetches: 0"#)],
                            &[T(r#"                          Index Searches: 50"#)],
                            &[T(r#"                          Worker 0:  actual rows=0.98 loops=50"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM k JOIN t ON t.a = k.a;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Workers Launched: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        ->  Nested Loop (actual rows=16057.00 loops=1)"#)],
                            &[T(r#"              ->  Seq Scan on k (actual rows=16384.00 loops=1)"#)],
                            &[T(r#"              ->  Memoize (actual rows=0.98 loops=16384)"#)],
                            &[T(r#"                    Cache Key: k.a"#)],
                            &[T(r#"                    Cache Mode: logical"#)],
                            &[T(r#"                    Worker 0:  Hits: 16334  Misses: 50  Evictions: 0  Overflows: 0  Memory Usage: 6kB"#)],
                            &[T(r#"                    ->  Index Only Scan using t_a on t (actual rows=0.98 loops=50)"#)],
                            &[T(r#"                          Index Cond: (a = k.a)"#)],
                            &[T(r#"                          Heap Fetches: 0"#)],
                            &[T(r#"                          Index Searches: 50"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashjoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_mergejoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Aggregate"#)],
                            &[T(r#"        ->  Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Result"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT (SELECT max(a) FROM s);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Aggregate"#)],
                            &[T(r#"          ->  Seq Scan on s"#)],
                            &[T(r#"  ->  Result"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT max(a) FROM s);",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT4)],
                        rows: &[
                            &[T("16384")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM s WHERE a < 10 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("4")],
                            &[T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET debug_parallel_query = regress;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate"#)],
                            &[T(r#"  ->  Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT a FROM s WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on public.s (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Output: a"#)],
                            &[T(r#"  Filter: (s.a = 1)"#)],
                            &[T(r#"  Rows Removed by Filter: 5461"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT (SELECT max(a) FROM s);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Aggregate"#)],
                            &[T(r#"          ->  Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 / (a - 1) FROM s WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET debug_parallel_query = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate"#)],
                            &[T(r#"  ->  Seq Scan on s"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET max_parallel_workers_per_gather;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT a FROM s WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Output: a"#)],
                            &[T(r#"  Workers Planned: 1"#)],
                            &[T(r#"  Workers Launched: 0"#)],
                            &[T(r#"  Single Copy: true"#)],
                            &[T(r#"  ->  Seq Scan on public.s (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Output: a"#)],
                            &[T(r#"        Filter: (s.a = 1)"#)],
                            &[T(r#"        Rows Removed by Filter: 5461"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5462")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET max_parallel_workers;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET debug_parallel_query;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "parallel query's debug_parallel_query = regress leaves plans and rows as they are",
            set_up_script: &[
                "CREATE TABLE n (a INT);",
                "INSERT INTO n VALUES (1);",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "CREATE TABLE ex_a (id INT PRIMARY KEY, x INT, y INT);",
                "CREATE TABLE ex_b (id INT PRIMARY KEY, x INT, y INT);",
                "INSERT INTO ex_a VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3), (4, NULL, 4), (5, 2, 5);",
                "INSERT INTO ex_b VALUES (1, 1, 1), (2, 2, 9), (3, 2, 2), (4, NULL, 4);",
                "CREATE TABLE fj_a (x INT);",
                "CREATE TABLE fj_b (y INT);",
                "INSERT INTO fj_a VALUES (1), (2);",
                "INSERT INTO fj_b VALUES (1), (3);",
                "CREATE TABLE sl_a (x INT PRIMARY KEY, v INT);",
                "CREATE TABLE sl_b (y INT, w INT);",
                "CREATE TABLE sl_c (z INT, u INT);",
                "INSERT INTO sl_a SELECT a, a % 7 FROM n WHERE a <= 200;",
                "INSERT INTO sl_b SELECT a % 50, a FROM n WHERE a <= 300;",
                "INSERT INTO sl_c SELECT a % 20, a % 3 FROM n WHERE a <= 100;",
                "SET debug_parallel_query = regress;",
                "EXPLAIN (COSTS OFF) SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE NOT EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x OR ex_b.y = ex_a.y) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x) AND NOT EXISTS (SELECT 1 FROM ex_b WHERE ex_b.y = ex_a.y) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM fj_a FULL JOIN fj_b ON fj_a.x = fj_b.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: fj_a.x, fj_b.y"#)],
                            &[T(r#"  ->  Merge Full Join"#)],
                            &[T(r#"        Merge Cond: (fj_a.x = fj_b.y)"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Sort Key: fj_a.x"#)],
                            &[T(r#"              ->  Seq Scan on fj_a"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Sort Key: fj_b.y"#)],
                            &[T(r#"              ->  Seq Scan on fj_b"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fj_a FULL JOIN fj_b ON fj_a.x = fj_b.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), Null],
                            &[Null, T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fj_a FULL JOIN fj_b ON fj_a.x < fj_b.y ORDER BY 1, 2;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FULL JOIN is only supported with merge-joinable or hash-joinable join conditions", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM fj_a LEFT JOIN fj_b ON fj_a.x = fj_b.y WHERE false;",
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
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM sl_a JOIN sl_b ON sl_a.x = sl_b.y AND sl_b.w IN (SELECT z FROM sl_c);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate"#)],
                            &[T(r#"  ->  Hash Join"#)],
                            &[T(r#"        Hash Cond: (sl_b.y = sl_a.x)"#)],
                            &[T(r#"        ->  Hash Join"#)],
                            &[T(r#"              Hash Cond: (sl_b.w = sl_c.z)"#)],
                            &[T(r#"              ->  Seq Scan on sl_b"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  HashAggregate"#)],
                            &[T(r#"                          Group Key: sl_c.z"#)],
                            &[T(r#"                          ->  Seq Scan on sl_c"#)],
                            &[T(r#"        ->  Hash"#)],
                            &[T(r#"              ->  Seq Scan on sl_a"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a JOIN sl_b ON sl_a.x = sl_b.y AND sl_b.w IN (SELECT z FROM sl_c);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("19")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a LEFT JOIN sl_b ON sl_a.x = sl_b.y AND EXISTS (SELECT 1 FROM sl_c WHERE sl_c.z = sl_b.w);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a LEFT JOIN sl_b ON sl_a.x = sl_b.y AND EXISTS (SELECT 1 FROM sl_c WHERE sl_c.z = sl_a.v);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("445")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a WHERE EXISTS (SELECT 1 FROM sl_b WHERE sl_b.y = sl_a.x AND EXISTS (SELECT 1 FROM sl_c WHERE sl_c.z = sl_b.w AND sl_c.u = sl_a.v));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM sl_a WHERE v = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  ->  Seq Scan on sl_a (actual rows=29.00 loops=1)"#)],
                            &[T(r#"        Filter: (v = 3)"#)],
                            &[T(r#"        Rows Removed by Filter: 171"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v, count(*) FROM sl_a GROUP BY v ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("28")],
                            &[T("1"), T("29")],
                            &[T("2"), T("29")],
                            &[T("3"), T("29")],
                            &[T("4"), T("29")],
                            &[T("5"), T("28")],
                            &[T("6"), T("28")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT (SELECT max(x) FROM sl_a);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  InitPlan 2"#)],
                            &[T(r#"    ->  Result"#)],
                            &[T(r#"          InitPlan 1"#)],
                            &[T(r#"            ->  Limit"#)],
                            &[T(r#"                  ->  Index Only Scan Backward using sl_a_pkey on sl_a"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT max(x) FROM sl_a);",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT4)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x / (v - 1) FROM sl_a WHERE x = 1;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET debug_parallel_query;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_parallel_cancel_wakes_leader() {
    run_scripts(&[
        ScriptTest {
            name: "parallel query's Gather Merge leader wakes for a statement timeout while a worker waits for a lock",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT)",
                "INSERT INTO t VALUES (1, 1)",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "INSERT INTO t SELECT a + m, (a + m) % 10 FROM t, (SELECT max(a) AS m FROM t) x",
                "CREATE TABLE t2 (x INT)",
                "INSERT INTO t2 VALUES (1)",
                "CREATE FUNCTION f_reads_t2(x INT) RETURNS INT LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN RETURN x + (SELECT count(*) FROM t2); END $$",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ LOCK TABLE t2 IN ACCESS EXCLUSIVE MODE",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_setup_cost = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_tuple_cost = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET min_parallel_table_scan_size = 0",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET parallel_leader_participation = off",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ EXPLAIN (COSTS OFF) SELECT a FROM t WHERE f_reads_t2(a) > 0 ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Gather Merge"#)],
                            &[T(r#"  Workers Planned: 2"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: a"#)],
                            &[T(r#"        ->  Parallel Seq Scan on t"#)],
                            &[T(r#"              Filter: (f_reads_t2(a) > 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SET statement_timeout = 200",
                    expected: Expected::Tag("SET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM (SELECT a FROM t WHERE f_reads_t2(a) > 0 ORDER BY a) s",
                    expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to statement timeout", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ RESET statement_timeout",
                    expected: Expected::Tag("RESET"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM (SELECT a FROM t WHERE f_reads_t2(a) > 0 ORDER BY a) s",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1024")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A parallel index build under a dropped index's name",
            set_up_script: &[
                "CREATE TABLE ts (a tsvector)",
                "INSERT INTO ts SELECT to_tsvector('simple', 'w' || g || ' q' || (g % 100)) FROM generate_series(1, 2000) g",
                "ALTER TABLE ts SET (parallel_workers = 2)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX ts_a ON ts USING gist (a)",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX ts_a",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ts_a ON ts USING gin (a)",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ts WHERE a @@ 'q7'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
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
fn test_parallel_numeric_variance() {
    run_scripts(&[
        ScriptTest {
            name: "parallel numeric variance passes sums of squares outside numeric's range between workers",
            set_up_script: &[
                "CREATE TABLE num_variance (a numeric) WITH (parallel_workers = 4);",
                "INSERT INTO num_variance VALUES (0), (3e-500), (-3e-500), (4e-500 - 1e-16383), (-4e-500 + 1e-16383);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL parallel_setup_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL max_parallel_workers_per_gather = 4;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT variance(a) FROM num_variance;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 4"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Seq Scan on num_variance"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT trim_scale(variance(a) * 1e1000) FROM num_variance;",
                    expected: Expected::Rows {
                        columns: &[Column("trim_scale", NUMERIC)],
                        rows: &[
                            &[T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM num_variance;",
                    expected: Expected::Tag("DELETE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO num_variance SELECT 9e131071 + x FROM generate_series(1, 5) x;",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT variance(a) FROM num_variance;",
                    expected: Expected::Rows {
                        columns: &[Column("variance", NUMERIC)],
                        rows: &[
                            &[T("2.5000000000000000")],
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
            ],
            ..S
        },
    ]);
}
