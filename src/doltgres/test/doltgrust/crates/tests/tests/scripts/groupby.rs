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
fn test_group_by() {
    run_scripts(&[
        ScriptTest {
            name: "Basic order by/group by cases",
            set_up_script: &[
                "create table members (id bigint primary key, team text);",
                "insert into members values (3,'red'), (4,'red'),(5,'orange'),(6,'orange'),(7,'orange'),(8,'purple');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select team as f from members order by id, f",
                    expected: Expected::Rows {
                        columns: &[Column("f", TEXT)],
                        rows: &[
                            &[T("red")],
                            &[T("red")],
                            &[T("orange")],
                            &[T("orange")],
                            &[T("orange")],
                            &[T("purple")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT team, COUNT(*) FROM members GROUP BY team ORDER BY 2",
                    expected: Expected::Rows {
                        columns: &[Column("team", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("purple"), T("1")],
                            &[T("red"), T("2")],
                            &[T("orange"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT team, COUNT(*) FROM members GROUP BY 1 ORDER BY 2",
                    expected: Expected::Rows {
                        columns: &[Column("team", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("purple"), T("1")],
                            &[T("red"), T("2")],
                            &[T("orange"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT team, COUNT(*) FROM members GROUP BY team ORDER BY columndoesnotexist",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "columndoesnotexist" does not exist"#, position: 59, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT t1.id as id FROM members AS t1 JOIN members AS t2 ON t1.id = t2.id WHERE t2.id > 0 ORDER BY t1.id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id as alias1, (SELECT alias1+1 group by alias1 having alias1 > 0) FROM members where id < 6;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "alias1" does not exist"#, position: 30, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, (SELECT UPPER(team) having id > 3) as upper_team FROM members where id < 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("upper_team", TEXT)],
                        rows: &[
                            &[T("3"), Null],
                            &[T("4"), T("RED")],
                            &[T("5"), T("ORANGE")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, (SELECT -1 as id having id < 10) as upper_team FROM members where id < 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("upper_team", INT4)],
                        rows: &[
                            &[T("3"), T("-1")],
                            &[T("4"), T("-1")],
                            &[T("5"), T("-1")],
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
fn test_outer_aggregate_rules() {
    run_scripts(&[
        ScriptTest {
            name: "aggregates of outer query columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ta (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ta VALUES (1),(2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select avg((select avg(a1.a order by (select avg(a2.a) from ta a3)) from ta a1)) from ta a2;",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate function calls cannot be nested", position: 46, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT max(ta.a)) FROM ta;",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT max(x.a) FROM ta x) FROM ta;",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select max(min(a)) from ta;",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate function calls cannot be nested", position: 12, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_grouping_sets() {
    run_scripts(&[
        ScriptTest {
            name: "grouping sets, rollup, cube, and grouping",
            set_up_script: &[
                "CREATE TABLE gs (a INT, b INT, v INT);",
                "INSERT INTO gs VALUES (1, 1, 10), (1, 2, 20), (2, 1, 30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a, b, grouping(a, b), sum(v) FROM gs GROUP BY ROLLUP (a, b) ORDER BY 3, 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("grouping", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("0"), T("10")],
                            &[T("1"), T("2"), T("0"), T("20")],
                            &[T("2"), T("1"), T("0"), T("30")],
                            &[T("1"), Null, T("1"), T("30")],
                            &[T("2"), Null, T("1"), T("30")],
                            &[Null, Null, T("3"), T("60")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, count(*) FROM gs GROUP BY CUBE (a, b) ORDER BY a NULLS FIRST, b NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("count", INT8)],
                        rows: &[
                            &[Null, Null, T("3")],
                            &[Null, T("1"), T("2")],
                            &[Null, T("2"), T("1")],
                            &[T("1"), Null, T("2")],
                            &[T("1"), T("1"), T("1")],
                            &[T("1"), T("2"), T("1")],
                            &[T("2"), Null, T("1")],
                            &[T("2"), T("1"), T("1")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, sum(v) FROM gs GROUP BY GROUPING SETS ((a), (b), ()) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), Null, T("30")],
                            &[T("2"), Null, T("30")],
                            &[Null, T("1"), T("40")],
                            &[Null, T("2"), T("20")],
                            &[Null, Null, T("60")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM gs WHERE false GROUP BY ();",
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
                    query: "SELECT a, count(*) FROM gs GROUP BY DISTINCT ROLLUP (a), ROLLUP (a) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("1")],
                            &[Null, T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grouping(v) FROM gs GROUP BY a;",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "arguments to GROUPING must be grouping expressions of the associated query level", position: 17, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_grouping_set_strategies() {
    run_scripts(&[
        ScriptTest {
            name: "grouping sets are computed by sorted rollups, hash tables, or both, with GROUPING() and HAVING, over empty input and in rescans",
            set_up_script: &[
                "CREATE TABLE gt (id INT PRIMARY KEY, a INT, b INT, c TEXT, v INT);",
                "INSERT INTO gt SELECT i, i % 3, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 4 END, 'c' || (i % 2), i FROM generate_series(1, 200) i;",
                "ANALYZE gt;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b, sum(v) FROM gt GROUP BY ROLLUP (a, b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("MixedAggregate")],
                            &[T("  Hash Key: a, b")],
                            &[T("  Hash Key: a")],
                            &[T("  Group Key: ()")],
                            &[T("  ->  Seq Scan on gt")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b, sum(v) FROM gt GROUP BY CUBE (a, b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("MixedAggregate")],
                            &[T("  Hash Key: a, b")],
                            &[T("  Hash Key: a")],
                            &[T("  Hash Key: b")],
                            &[T("  Group Key: ()")],
                            &[T("  ->  Seq Scan on gt")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b, c, count(*) FROM gt GROUP BY GROUPING SETS ((a), (b), (c), ());",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("MixedAggregate")],
                            &[T("  Hash Key: a")],
                            &[T("  Hash Key: b")],
                            &[T("  Hash Key: c")],
                            &[T("  Group Key: ()")],
                            &[T("  ->  Seq Scan on gt")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM gt GROUP BY GROUPING SETS ((), ());",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  Group Key: ()")],
                            &[T("  Group Key: ()")],
                            &[T("  ->  Seq Scan on gt")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT 1 FROM gt GROUP BY GROUPING SETS ((), ()) HAVING 1 = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Result")],
                            &[T("  ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b, count(*) FROM gt GROUP BY ROLLUP (a, b) HAVING a > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("MixedAggregate")],
                            &[T("  Hash Key: a, b")],
                            &[T("  Hash Key: a")],
                            &[T("  Group Key: ()")],
                            &[T("  Filter: (a > 0)")],
                            &[T("  ->  Seq Scan on gt")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, sum(v), count(*), GROUPING(a, b) FROM gt GROUP BY ROLLUP (a, b) ORDER BY 1, 2, 5;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("sum", INT8), Column("count", INT8), Column("grouping", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("1500"), T("15"), T("0")],
                            &[T("0"), T("1"), T("1386"), T("14"), T("0")],
                            &[T("0"), T("2"), T("1470"), T("15"), T("0")],
                            &[T("0"), T("3"), T("1584"), T("16"), T("0")],
                            &[T("0"), Null, T("693"), T("6"), T("0")],
                            &[T("0"), Null, T("6633"), T("66"), T("1")],
                            &[T("1"), T("0"), T("1612"), T("16"), T("0")],
                            &[T("1"), T("1"), T("1528"), T("16"), T("0")],
                            &[T("1"), T("2"), T("1424"), T("14"), T("0")],
                            &[T("1"), T("3"), T("1509"), T("15"), T("0")],
                            &[T("1"), Null, T("627"), T("6"), T("0")],
                            &[T("1"), Null, T("6700"), T("67"), T("1")],
                            &[T("2"), T("0"), T("1548"), T("15"), T("0")],
                            &[T("2"), T("1"), T("1640"), T("16"), T("0")],
                            &[T("2"), T("2"), T("1556"), T("16"), T("0")],
                            &[T("2"), T("3"), T("1462"), T("14"), T("0")],
                            &[T("2"), Null, T("561"), T("6"), T("0")],
                            &[T("2"), Null, T("6767"), T("67"), T("1")],
                            &[Null, Null, T("20100"), T("200"), T("3")],
                        ],
                        tag: "SELECT 19",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, c, count(*), GROUPING(a), GROUPING(a, b, c) FROM gt GROUP BY CUBE (a, b, c) ORDER BY 1, 2, 3, 6;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("count", INT8), Column("grouping", INT4), Column("grouping", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("c0"), T("15"), T("0"), T("0")],
                            &[T("0"), T("0"), Null, T("15"), T("0"), T("1")],
                            &[T("0"), T("1"), T("c1"), T("14"), T("0"), T("0")],
                            &[T("0"), T("1"), Null, T("14"), T("0"), T("1")],
                            &[T("0"), T("2"), T("c0"), T("15"), T("0"), T("0")],
                            &[T("0"), T("2"), Null, T("15"), T("0"), T("1")],
                            &[T("0"), T("3"), T("c1"), T("16"), T("0"), T("0")],
                            &[T("0"), T("3"), Null, T("16"), T("0"), T("1")],
                            &[T("0"), Null, T("c0"), T("3"), T("0"), T("0")],
                            &[T("0"), Null, T("c0"), T("33"), T("0"), T("2")],
                            &[T("0"), Null, T("c1"), T("3"), T("0"), T("0")],
                            &[T("0"), Null, T("c1"), T("33"), T("0"), T("2")],
                            &[T("0"), Null, Null, T("6"), T("0"), T("1")],
                            &[T("0"), Null, Null, T("66"), T("0"), T("3")],
                            &[T("1"), T("0"), T("c0"), T("16"), T("0"), T("0")],
                            &[T("1"), T("0"), Null, T("16"), T("0"), T("1")],
                            &[T("1"), T("1"), T("c1"), T("16"), T("0"), T("0")],
                            &[T("1"), T("1"), Null, T("16"), T("0"), T("1")],
                            &[T("1"), T("2"), T("c0"), T("14"), T("0"), T("0")],
                            &[T("1"), T("2"), Null, T("14"), T("0"), T("1")],
                            &[T("1"), T("3"), T("c1"), T("15"), T("0"), T("0")],
                            &[T("1"), T("3"), Null, T("15"), T("0"), T("1")],
                            &[T("1"), Null, T("c0"), T("3"), T("0"), T("0")],
                            &[T("1"), Null, T("c0"), T("33"), T("0"), T("2")],
                            &[T("1"), Null, T("c1"), T("3"), T("0"), T("0")],
                            &[T("1"), Null, T("c1"), T("34"), T("0"), T("2")],
                            &[T("1"), Null, Null, T("6"), T("0"), T("1")],
                            &[T("1"), Null, Null, T("67"), T("0"), T("3")],
                            &[T("2"), T("0"), T("c0"), T("15"), T("0"), T("0")],
                            &[T("2"), T("0"), Null, T("15"), T("0"), T("1")],
                            &[T("2"), T("1"), T("c1"), T("16"), T("0"), T("0")],
                            &[T("2"), T("1"), Null, T("16"), T("0"), T("1")],
                            &[T("2"), T("2"), T("c0"), T("16"), T("0"), T("0")],
                            &[T("2"), T("2"), Null, T("16"), T("0"), T("1")],
                            &[T("2"), T("3"), T("c1"), T("14"), T("0"), T("0")],
                            &[T("2"), T("3"), Null, T("14"), T("0"), T("1")],
                            &[T("2"), Null, T("c0"), T("3"), T("0"), T("0")],
                            &[T("2"), Null, T("c0"), T("34"), T("0"), T("2")],
                            &[T("2"), Null, T("c1"), T("3"), T("0"), T("0")],
                            &[T("2"), Null, T("c1"), T("33"), T("0"), T("2")],
                            &[T("2"), Null, Null, T("6"), T("0"), T("1")],
                            &[T("2"), Null, Null, T("67"), T("0"), T("3")],
                            &[Null, T("0"), T("c0"), T("46"), T("1"), T("4")],
                            &[Null, T("0"), Null, T("46"), T("1"), T("5")],
                            &[Null, T("1"), T("c1"), T("46"), T("1"), T("4")],
                            &[Null, T("1"), Null, T("46"), T("1"), T("5")],
                            &[Null, T("2"), T("c0"), T("45"), T("1"), T("4")],
                            &[Null, T("2"), Null, T("45"), T("1"), T("5")],
                            &[Null, T("3"), T("c1"), T("45"), T("1"), T("4")],
                            &[Null, T("3"), Null, T("45"), T("1"), T("5")],
                            &[Null, Null, T("c0"), T("9"), T("1"), T("4")],
                            &[Null, Null, T("c0"), T("100"), T("1"), T("6")],
                            &[Null, Null, T("c1"), T("9"), T("1"), T("4")],
                            &[Null, Null, T("c1"), T("100"), T("1"), T("6")],
                            &[Null, Null, Null, T("18"), T("1"), T("5")],
                            &[Null, Null, Null, T("200"), T("1"), T("7")],
                        ],
                        tag: "SELECT 56",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, string_agg(c, ',' ORDER BY id DESC) FILTER (WHERE id < 20), count(DISTINCT b) FROM gt GROUP BY ROLLUP (a) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("string_agg", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("c0,c1,c0,c1,c0,c1"), T("4")],
                            &[T("1"), T("c1,c0,c1,c0,c1,c0,c1"), T("4")],
                            &[T("2"), T("c1,c0,c1,c0,c1,c0"), T("4")],
                            &[Null, T("c1,c0,c1,c0,c1,c0,c1,c0,c1,c0,c1,c0,c1,c0,c1,c0,c1,c0,c1"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, sum(v) FROM gt GROUP BY a, ROLLUP (b) HAVING sum(v) > 2000 ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), Null, T("6633")],
                            &[T("1"), Null, T("6700")],
                            &[T("2"), Null, T("6767")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM gt WHERE false GROUP BY GROUPING SETS ((), ());",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                            &[T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, count(*) FROM gt WHERE false GROUP BY ROLLUP (a);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[Null, T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashagg = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, c, count(*), GROUPING(a, b, c) FROM gt GROUP BY GROUPING SETS ((a, b), (a), (b, c), (c), ()) ORDER BY 1, 2, 3, 5;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("count", INT8), Column("grouping", INT4)],
                        rows: &[
                            &[T("0"), T("0"), Null, T("15"), T("1")],
                            &[T("0"), T("1"), Null, T("14"), T("1")],
                            &[T("0"), T("2"), Null, T("15"), T("1")],
                            &[T("0"), T("3"), Null, T("16"), T("1")],
                            &[T("0"), Null, Null, T("6"), T("1")],
                            &[T("0"), Null, Null, T("66"), T("3")],
                            &[T("1"), T("0"), Null, T("16"), T("1")],
                            &[T("1"), T("1"), Null, T("16"), T("1")],
                            &[T("1"), T("2"), Null, T("14"), T("1")],
                            &[T("1"), T("3"), Null, T("15"), T("1")],
                            &[T("1"), Null, Null, T("6"), T("1")],
                            &[T("1"), Null, Null, T("67"), T("3")],
                            &[T("2"), T("0"), Null, T("15"), T("1")],
                            &[T("2"), T("1"), Null, T("16"), T("1")],
                            &[T("2"), T("2"), Null, T("16"), T("1")],
                            &[T("2"), T("3"), Null, T("14"), T("1")],
                            &[T("2"), Null, Null, T("6"), T("1")],
                            &[T("2"), Null, Null, T("67"), T("3")],
                            &[Null, T("0"), T("c0"), T("46"), T("4")],
                            &[Null, T("1"), T("c1"), T("46"), T("4")],
                            &[Null, T("2"), T("c0"), T("45"), T("4")],
                            &[Null, T("3"), T("c1"), T("45"), T("4")],
                            &[Null, Null, T("c0"), T("9"), T("4")],
                            &[Null, Null, T("c0"), T("100"), T("6")],
                            &[Null, Null, T("c1"), T("9"), T("4")],
                            &[Null, Null, T("c1"), T("100"), T("6")],
                            &[Null, Null, Null, T("200"), T("7")],
                        ],
                        tag: "SELECT 27",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashagg;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_sort = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b, c, count(*) FROM gt GROUP BY GROUPING SETS ((a, b), (a), (b, c), (c), ());",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("MixedAggregate")],
                            &[T("  Hash Key: a, b")],
                            &[T("  Hash Key: a")],
                            &[T("  Hash Key: c, b")],
                            &[T("  Hash Key: c")],
                            &[T("  Group Key: ()")],
                            &[T("  ->  Seq Scan on gt")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, c, count(*), GROUPING(a, b, c) FROM gt GROUP BY GROUPING SETS ((a, b), (a), (b, c), (c), ()) ORDER BY 1, 2, 3, 5;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("count", INT8), Column("grouping", INT4)],
                        rows: &[
                            &[T("0"), T("0"), Null, T("15"), T("1")],
                            &[T("0"), T("1"), Null, T("14"), T("1")],
                            &[T("0"), T("2"), Null, T("15"), T("1")],
                            &[T("0"), T("3"), Null, T("16"), T("1")],
                            &[T("0"), Null, Null, T("6"), T("1")],
                            &[T("0"), Null, Null, T("66"), T("3")],
                            &[T("1"), T("0"), Null, T("16"), T("1")],
                            &[T("1"), T("1"), Null, T("16"), T("1")],
                            &[T("1"), T("2"), Null, T("14"), T("1")],
                            &[T("1"), T("3"), Null, T("15"), T("1")],
                            &[T("1"), Null, Null, T("6"), T("1")],
                            &[T("1"), Null, Null, T("67"), T("3")],
                            &[T("2"), T("0"), Null, T("15"), T("1")],
                            &[T("2"), T("1"), Null, T("16"), T("1")],
                            &[T("2"), T("2"), Null, T("16"), T("1")],
                            &[T("2"), T("3"), Null, T("14"), T("1")],
                            &[T("2"), Null, Null, T("6"), T("1")],
                            &[T("2"), Null, Null, T("67"), T("3")],
                            &[Null, T("0"), T("c0"), T("46"), T("4")],
                            &[Null, T("1"), T("c1"), T("46"), T("4")],
                            &[Null, T("2"), T("c0"), T("45"), T("4")],
                            &[Null, T("3"), T("c1"), T("45"), T("4")],
                            &[Null, Null, T("c0"), T("9"), T("4")],
                            &[Null, Null, T("c0"), T("100"), T("6")],
                            &[Null, Null, T("c1"), T("9"), T("4")],
                            &[Null, Null, T("c1"), T("100"), T("6")],
                            &[Null, Null, Null, T("200"), T("7")],
                        ],
                        tag: "SELECT 27",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_sort;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g.x, (SELECT sum(v) FROM gt WHERE gt.a = g.x GROUP BY GROUPING SETS ((), (b)) ORDER BY 1 DESC LIMIT 1) FROM generate_series(0, 2) g(x);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), T("6633")],
                            &[T("1"), T("6700")],
                            &[T("2"), T("6767")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.l, s.a, s.n FROM (VALUES (50), (100)) r(l), LATERAL (SELECT a, count(*) AS n FROM gt WHERE id < r.l GROUP BY ROLLUP (a)) s ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("l", INT4), Column("a", INT4), Column("n", INT8)],
                        rows: &[
                            &[T("50"), T("0"), T("16")],
                            &[T("50"), T("1"), T("17")],
                            &[T("50"), T("2"), T("16")],
                            &[T("50"), Null, T("49")],
                            &[T("100"), T("0"), T("33")],
                            &[T("100"), T("1"), T("33")],
                            &[T("100"), T("2"), T("33")],
                            &[T("100"), Null, T("99")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT a, b, c, count(*) FROM gt GROUP BY GROUPING SETS ((a, b), (a), (b, c), (c), ());",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("MixedAggregate (actual rows=27.00 loops=1)")],
                            &[T("  Hash Key: a, b")],
                            &[T("  Hash Key: a")],
                            &[T("  Hash Key: c, b")],
                            &[T("  Hash Key: c")],
                            &[T("  Group Key: ()")],
                            &[T("  Batches: 1  Memory Usage: 64kB")],
                            &[T("  ->  Seq Scan on gt (actual rows=200.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hashed grouping sets spill each set's rows past work_mem to its own partitions",
            set_up_script: &[
                "CREATE TABLE gb2 (g INT PRIMARY KEY, m INT, s INT, t TEXT);",
                "INSERT INTO gb2 SELECT g, g % 3000, g % 7, 'x' || (g % 1500) FROM generate_series(1, 30000) g;",
                "CREATE FUNCTION gbf() RETURNS SETOF gb2 LANGUAGE plpgsql ROWS 10 AS $$ BEGIN RETURN QUERY SELECT * FROM gb2; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT m, s, count(*) FROM gbf() GROUP BY GROUPING SETS ((m), (s));",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=3007.00 loops=1)")],
                            &[T("  Hash Key: m")],
                            &[T("  Hash Key: s")],
                            &[T("  Batches: 5  Memory Usage: 193kB  Disk Usage: 712kB")],
                            &[T("  ->  Function Scan on gbf (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(n), sum(coalesce(m, 0)), sum(coalesce(s, 0)) FROM (SELECT m, s, count(*) AS n FROM gbf() GROUP BY GROUPING SETS ((m), (s))) q;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", NUMERIC), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("3007"), T("60000"), T("4498500"), T("21")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT m, t, sum(g), GROUPING(m, t) FROM gbf() GROUP BY GROUPING SETS ((m), (t), ());",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("MixedAggregate (actual rows=4501.00 loops=1)")],
                            &[T("  Hash Key: m")],
                            &[T("  Hash Key: t")],
                            &[T("  Group Key: ()")],
                            &[T("  Batches: 33  Memory Usage: 209kB  Disk Usage: 2312kB")],
                            &[T("  ->  Function Scan on gbf (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(sg), sum(gr) FROM (SELECT m, t, sum(g) AS sg, GROUPING(m, t) AS gr FROM gbf() GROUP BY GROUPING SETS ((m), (t), ())) q;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", NUMERIC), Column("sum", INT8)],
                        rows: &[
                            &[T("4501"), T("1350045000"), T("6003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT m, s, t, max(t) FROM gbf() GROUP BY CUBE (m, s, t);",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("MixedAggregate (actual rows=60008.00 loops=1)")],
                            &[T("  Hash Key: m, s, t")],
                            &[T("  Hash Key: m, s")],
                            &[T("  Hash Key: m")],
                            &[T("  Hash Key: s, t")],
                            &[T("  Hash Key: s")],
                            &[T("  Hash Key: t, m")],
                            &[T("  Hash Key: t")],
                            &[T("  Group Key: ()")],
                            &[T("  Batches: 59831  Memory Usage: 360kB  Disk Usage: 10536kB")],
                            &[T("  ->  Function Scan on gbf (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(length(mt)) FROM (SELECT m, s, t, max(t) AS mt FROM gbf() GROUP BY CUBE (m, s, t)) q;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("60008"), T("255632")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_min_max_over_union_all_with_outer_columns() {
    run_scripts(&[
        ScriptTest {
            name: "a min/max index path over a UNION ALL reads the outer query's columns at the right level",
            set_up_script: &[
                "CREATE TABLE mm1 (a INT) WITH (autovacuum_enabled = false);",
                "CREATE TABLE mm2 (b INT) WITH (autovacuum_enabled = false);",
                "CREATE TABLE mmo (k INT) WITH (autovacuum_enabled = false);",
                "INSERT INTO mm1 SELECT g FROM generate_series(1, 1000) g;",
                "INSERT INTO mm2 SELECT g * 2 FROM generate_series(1, 1000) g;",
                "INSERT INTO mmo VALUES (10), (500), (3000);",
                "CREATE INDEX ON mm1 (a);",
                "CREATE INDEX ON mm2 (b);",
                "ANALYZE mm1, mm2, mmo;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT k, (SELECT max(x) FROM (SELECT a AS x, mmo.k AS y FROM mm1 UNION ALL SELECT b, mmo.k FROM mm2) u WHERE x < y) FROM mmo ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("max", INT4)],
                        rows: &[
                            &[T("10"), T("9")],
                            &[T("500"), T("499")],
                            &[T("3000"), T("2000")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT k, (SELECT max(x) FROM (SELECT a AS x, mmo.k AS y FROM mm1 UNION ALL SELECT b, mmo.k FROM mm2) u WHERE x < y) FROM mmo ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: mmo.k")],
                            &[T("  ->  Seq Scan on mmo")],
                            &[T("        SubPlan 2")],
                            &[T("          ->  Result")],
                            &[T("                InitPlan 1")],
                            &[T("                  ->  Limit")],
                            &[T("                        ->  Merge Append")],
                            &[T("                              Sort Key: mm1.a DESC")],
                            &[T("                              ->  Index Only Scan Backward using mm1_a_idx on mm1")],
                            &[T("                                    Index Cond: ((a IS NOT NULL) AND (a < mmo.k))")],
                            &[T("                              ->  Index Only Scan Backward using mm2_b_idx on mm2")],
                            &[T("                                    Index Cond: ((b IS NOT NULL) AND (b < mmo.k))")],
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
