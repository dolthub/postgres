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
fn test_window_functions() {
    run_scripts(&[
        ScriptTest {
            name: "native sum and row_number as window functions",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, grp INT, amt INT);",
                "INSERT INTO t VALUES (1, 1, 10), (2, 1, 20), (3, 1, 30), (4, 2, 5), (5, 2, 15);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, sum(amt) OVER (PARTITION BY grp ORDER BY id) FROM t ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("30")],
                            &[T("3"), T("60")],
                            &[T("4"), T("5")],
                            &[T("5"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, row_number() OVER (PARTITION BY grp ORDER BY id) FROM t ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("row_number", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("1")],
                            &[T("5"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grp, sum(amt) FROM t GROUP BY grp ORDER BY grp",
                    expected: Expected::Rows {
                        columns: &[Column("grp", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("60")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "distinct window aggregates are unsupported",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(DISTINCT *) OVER () FROM (VALUES (1)) AS t(v)",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "*""#, position: 23, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(DISTINCT v) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) FROM (VALUES (1, 1), (2, 1), (3, 2)) AS t(id, v)",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DISTINCT is not implemented for window functions", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(DISTINCT v) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM (VALUES (1, 1), (2, 1), (3, 2)) AS t(id, v)",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DISTINCT is not implemented for window functions", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT avg(DISTINCT v) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) FROM (VALUES (1, 1.00::numeric), (2, 1.00::numeric), (3, 4.00::numeric)) AS t(id, v)",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DISTINCT is not implemented for window functions", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT min(DISTINCT v) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM (VALUES (1, 1), (2, 1), (3, 2)) AS t(id, v)",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DISTINCT is not implemented for window functions", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT max(DISTINCT v) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM (VALUES (1, 1), (2, 1), (3, 2)) AS t(id, v)",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DISTINCT is not implemented for window functions", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic window functions",
            set_up_script: &[
                "CREATE TABLE c (c_id INT PRIMARY KEY, bill TEXT);",
                "CREATE TABLE o (o_id INT PRIMARY KEY, c_id INT, ship TEXT);",
                "INSERT INTO c VALUES (1, 'CA'), (2, 'TX'), (3, 'MA'), (4, 'TX'), (5, NULL), (6, 'FL');",
                "INSERT INTO o VALUES (10, 1, 'CA'), (20, 1, 'CA'), (30, 1, 'CA'), (40, 2, 'CA'), (50, 2, 'TX'), (60, 2, NULL), (70, 4, 'WY'), (80, 4, NULL), (90, 6, 'WA');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT row_number() OVER () AS rn FROM o WHERE c_id=-999",
                    expected: Expected::Rows {
                        columns: &[Column("rn", INT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_number() OVER () AS rn FROM o WHERE c_id=1",
                    expected: Expected::Rows {
                        columns: &[Column("rn", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rank() OVER () AS rnk FROM o WHERE c_id=-999",
                    expected: Expected::Rows {
                        columns: &[Column("rnk", INT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT o_id, c_id, rank() OVER (ORDER BY o_id) AS rnk FROM o WHERE c_id=1",
                    expected: Expected::Rows {
                        columns: &[Column("o_id", INT4), Column("c_id", INT4), Column("rnk", INT8)],
                        rows: &[
                            &[T("10"), T("1"), T("1")],
                            &[T("20"), T("1"), T("2")],
                            &[T("30"), T("1"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dense_rank() OVER () AS drnk FROM o WHERE c_id=-999",
                    expected: Expected::Rows {
                        columns: &[Column("drnk", INT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ship, dense_rank() OVER (ORDER BY ship) AS drnk FROM o WHERE c_id IN (1, 2) ORDER BY ship",
                    expected: Expected::Rows {
                        columns: &[Column("ship", TEXT), Column("drnk", INT8)],
                        rows: &[
                            &[T("CA"), T("1")],
                            &[T("CA"), T("1")],
                            &[T("CA"), T("1")],
                            &[T("CA"), T("1")],
                            &[T("TX"), T("2")],
                            &[Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT c_id AS c_c_id, bill FROM c) sq1, LATERAL (SELECT row_number() OVER () AS rownum FROM o WHERE c_id = c_c_id) sq2 ORDER BY c_c_id, bill, rownum",
                    expected: Expected::Rows {
                        columns: &[Column("c_c_id", INT4), Column("bill", TEXT), Column("rownum", INT8)],
                        rows: &[
                            &[T("1"), T("CA"), T("1")],
                            &[T("1"), T("CA"), T("2")],
                            &[T("1"), T("CA"), T("3")],
                            &[T("2"), T("TX"), T("1")],
                            &[T("2"), T("TX"), T("2")],
                            &[T("2"), T("TX"), T("3")],
                            &[T("4"), T("TX"), T("1")],
                            &[T("4"), T("TX"), T("2")],
                            &[T("6"), T("FL"), T("1")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c_id, rank() OVER (ORDER BY c_id) AS rnk FROM c ORDER BY rnk DESC LIMIT 3",
                    expected: Expected::Rows {
                        columns: &[Column("c_id", INT4), Column("rnk", INT8)],
                        rows: &[
                            &[T("6"), T("6")],
                            &[T("5"), T("5")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c_id, rank() OVER (ORDER BY c_id) AS r FROM c ORDER BY r",
                    expected: Expected::Rows {
                        columns: &[Column("c_id", INT4), Column("r", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT c_id, rank() OVER (ORDER BY c_id) AS r FROM c ORDER BY r",
                    expected: Expected::Rows {
                        columns: &[Column("c_id", INT4), Column("r", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(CASE WHEN r > 0 THEN 1 ELSE 0 END) FROM (SELECT rank() OVER (ORDER BY c_id) AS r FROM c) t",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c_id, SUM(o_id) OVER (PARTITION BY c_id) AS s FROM o WHERE c_id = 1 ORDER BY o_id",
                    expected: Expected::Rows {
                        columns: &[Column("c_id", INT4), Column("s", INT8)],
                        rows: &[
                            &[T("1"), T("60")],
                            &[T("1"), T("60")],
                            &[T("1"), T("60")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "named window reference honors ORDER BY from the WINDOW clause",
            set_up_script: &[
                "CREATE TABLE t_named(id int, grp int, amt int);",
                "INSERT INTO t_named VALUES (1,1,10),(2,1,20),(3,2,5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, SUM(amt) OVER w AS s FROM t_named WINDOW w AS (PARTITION BY grp ORDER BY id) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("s", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("30")],
                            &[T("3"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, SUM(amt) OVER (PARTITION BY grp ORDER BY id) AS s FROM t_named ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("s", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("30")],
                            &[T("3"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "named window inheritance chain honors ORDER BY added by the child",
            set_up_script: &[
                "CREATE TABLE t_inherit(id int, grp int, amt int);",
                "INSERT INTO t_inherit VALUES (1,1,10),(2,1,20),(3,1,30),(4,2,5),(5,2,15);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, SUM(amt) OVER w2 AS s FROM t_inherit WINDOW w1 AS (PARTITION BY grp), w2 AS (w1 ORDER BY id) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("s", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("30")],
                            &[T("3"), T("60")],
                            &[T("4"), T("5")],
                            &[T("5"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, SUM(amt) OVER (w1 ORDER BY id) AS s FROM t_inherit WINDOW w1 AS (PARTITION BY grp) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("s", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("30")],
                            &[T("3"), T("60")],
                            &[T("4"), T("5")],
                            &[T("5"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, SUM(amt) OVER (PARTITION BY grp ORDER BY id) AS s FROM t_inherit ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("s", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("30")],
                            &[T("3"), T("60")],
                            &[T("4"), T("5")],
                            &[T("5"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "window SUM/AVG wrapped in a subquery projection",
            set_up_script: &[
                "CREATE TABLE wrapper_probe (grp INT, val INT);",
                "INSERT INTO wrapper_probe VALUES (1, 10), (1, 20), (2, 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT grp, val, grp_total FROM (SELECT grp, val, SUM(val) OVER (PARTITION BY grp) AS grp_total FROM wrapper_probe) sub ORDER BY grp, val;",
                    expected: Expected::Rows {
                        columns: &[Column("grp", INT4), Column("val", INT4), Column("grp_total", INT8)],
                        rows: &[
                            &[T("1"), T("10"), T("30")],
                            &[T("1"), T("20"), T("30")],
                            &[T("2"), T("5"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT grp, val, grp_avg FROM (SELECT grp, val, AVG(val) OVER (PARTITION BY grp) AS grp_avg FROM wrapper_probe) sub ORDER BY grp, val;",
                    expected: Expected::Rows {
                        columns: &[Column("grp", INT4), Column("val", INT4), Column("grp_avg", NUMERIC)],
                        rows: &[
                            &[T("1"), T("10"), T("15.0000000000000000")],
                            &[T("1"), T("20"), T("15.0000000000000000")],
                            &[T("2"), T("5"), T("5.0000000000000000")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "RANGE frame with INTERVAL month boundary is calendar-correct",
            set_up_script: &[
                "CREATE TABLE month_edge (d DATE, v INT);",
                "INSERT INTO month_edge VALUES ('2022-01-31', 1), ('2022-02-28', 2), ('2022-03-01', 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT sum(v) OVER (ORDER BY d RANGE BETWEEN UNBOUNDED PRECEDING AND INTERVAL '1' MONTH FOLLOWING) FROM month_edge ORDER BY d",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("3")],
                            &[T("6")],
                            &[T("6")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ntile and cume_dist ignore ties/frame and operate over the whole partition",
            set_up_script: &[
                "CREATE TABLE rank_ext (id INT PRIMARY KEY, grp INT, val INT);",
                "INSERT INTO rank_ext VALUES (1,1,10),(2,1,10),(3,1,20),(4,1,30),(5,2,5),(6,2,15);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, ntile(2) OVER (PARTITION BY grp ORDER BY val) FROM rank_ext ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ntile", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), T("2")],
                            &[T("5"), T("1")],
                            &[T("6"), T("2")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, cume_dist() OVER (PARTITION BY grp ORDER BY val) FROM rank_ext ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("cume_dist", FLOAT8)],
                        rows: &[
                            &[T("1"), T("0.5")],
                            &[T("2"), T("0.5")],
                            &[T("3"), T("0.75")],
                            &[T("4"), T("1")],
                            &[T("5"), T("0.5")],
                            &[T("6"), T("1")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cume_dist() FROM rank_ext",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "window function cume_dist requires an OVER clause", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_number() FROM rank_ext",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "window function row_number requires an OVER clause", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rank() FROM rank_ext",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "window function rank requires an OVER clause", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple differently-framed numeric RANGE windows in one SELECT don't collide",
            set_up_script: &[
                "CREATE TABLE boundary_2 (id INT PRIMARY KEY, val INT);",
                "INSERT INTO boundary_2 VALUES (1,10),(2,20),(3,30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT
					  sum(val) over (order by id range between 0 preceding and 0 following) as r0,
					  sum(val) over (order by id range between current row and 1 following) as r1foll,
					  sum(val) over (order by id range between unbounded preceding and current row) as runbndprec,
					  sum(val) over (order by id range between current row and unbounded following) as runbndfoll
					FROM boundary_2 ORDER BY id"#,
                    expected: Expected::Rows {
                        columns: &[Column("r0", INT8), Column("r1foll", INT8), Column("runbndprec", INT8), Column("runbndfoll", INT8)],
                        rows: &[
                            &[T("10"), T("30"), T("10"), T("60")],
                            &[T("20"), T("50"), T("30"), T("50")],
                            &[T("30"), T("30"), T("60"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_number() over (order by id) as rn1, row_number() over (order by id desc) as rn2 FROM boundary_2 ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("rn1", INT8), Column("rn2", INT8)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("2"), T("2")],
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nth_value respects the window's frame and returns the polymorphic argument type",
            set_up_script: &[
                "CREATE TABLE nv (id INT PRIMARY KEY, grp INT, val INT);",
                "INSERT INTO nv VALUES (1,1,100),(2,1,200),(3,1,300),(4,2,999);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, nth_value(val, 2) OVER (PARTITION BY grp ORDER BY id) FROM nv ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("nth_value", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("200")],
                            &[T("3"), T("200")],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, nth_value(val, 2) OVER (PARTITION BY grp ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM nv ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("nth_value", INT4)],
                        rows: &[
                            &[T("1"), T("200")],
                            &[T("2"), T("200")],
                            &[T("3"), T("200")],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "variance/stddev window functions over an int column",
            set_up_script: &[
                "CREATE TABLE t3038 (id BIGINT PRIMARY KEY, grp VARCHAR(10), val INT);",
                "INSERT INTO t3038 VALUES (1,'a',10), (2,'a',20), (3,'b',30), (4,'b',5), (5,'c',15), (6,'c',25);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, STDDEV_POP(val) OVER (ORDER BY grp) FROM t3038 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("stddev_pop", NUMERIC)],
                        rows: &[
                            &[T("1"), T("5.0000000000000000")],
                            &[T("2"), T("5.0000000000000000")],
                            &[T("3"), T("9.6014321848357602")],
                            &[T("4"), T("9.6014321848357602")],
                            &[T("5"), T("8.5391256382996653")],
                            &[T("6"), T("8.5391256382996653")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, STDDEV_SAMP(val) OVER (ORDER BY grp) FROM t3038 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("stddev_samp", NUMERIC)],
                        rows: &[
                            &[T("1"), T("7.0710678118654752")],
                            &[T("2"), T("7.0710678118654752")],
                            &[T("3"), T("11.0867789130417256")],
                            &[T("4"), T("11.0867789130417256")],
                            &[T("5"), T("9.3541434669348535")],
                            &[T("6"), T("9.3541434669348535")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, VAR_POP(val) OVER (ORDER BY grp) FROM t3038 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("var_pop", NUMERIC)],
                        rows: &[
                            &[T("1"), T("25.0000000000000000")],
                            &[T("2"), T("25.0000000000000000")],
                            &[T("3"), T("92.1875000000000000")],
                            &[T("4"), T("92.1875000000000000")],
                            &[T("5"), T("72.9166666666666667")],
                            &[T("6"), T("72.9166666666666667")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, VAR_SAMP(val) OVER (ORDER BY grp) FROM t3038 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("var_samp", NUMERIC)],
                        rows: &[
                            &[T("1"), T("50.0000000000000000")],
                            &[T("2"), T("50.0000000000000000")],
                            &[T("3"), T("122.9166666666666667")],
                            &[T("4"), T("122.9166666666666667")],
                            &[T("5"), T("87.5000000000000000")],
                            &[T("6"), T("87.5000000000000000")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "variance/stddev as GROUP BY aggregates, single-row, float8, and aliases",
            set_up_script: &[
                "CREATE TABLE t3038b (grp VARCHAR(10), val INT);",
                "INSERT INTO t3038b VALUES ('a',10), ('a',20), ('b',30), ('b',5), ('c',15), ('c',25);",
                "CREATE TABLE t3038_one (val INT);",
                "INSERT INTO t3038_one VALUES (42);",
                "CREATE TABLE t3038_f (val DOUBLE PRECISION);",
                "INSERT INTO t3038_f VALUES (10.0), (20.0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT grp, VAR_POP(val), VAR_SAMP(val), STDDEV_POP(val), STDDEV_SAMP(val) FROM t3038b GROUP BY grp ORDER BY grp;",
                    expected: Expected::Rows {
                        columns: &[Column("grp", VARCHAR), Column("var_pop", NUMERIC), Column("var_samp", NUMERIC), Column("stddev_pop", NUMERIC), Column("stddev_samp", NUMERIC)],
                        rows: &[
                            &[T("a"), T("25.0000000000000000"), T("50.0000000000000000"), T("5.0000000000000000"), T("7.0710678118654752")],
                            &[T("b"), T("156.2500000000000000"), T("312.5000000000000000"), T("12.5000000000000000"), T("17.6776695296636881")],
                            &[T("c"), T("25.0000000000000000"), T("50.0000000000000000"), T("5.0000000000000000"), T("7.0710678118654752")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT VAR_POP(val), VAR_SAMP(val), STDDEV_POP(val), STDDEV_SAMP(val) FROM t3038_one;",
                    expected: Expected::Rows {
                        columns: &[Column("var_pop", NUMERIC), Column("var_samp", NUMERIC), Column("stddev_pop", NUMERIC), Column("stddev_samp", NUMERIC)],
                        rows: &[
                            &[T("0"), Null, T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT VAR_POP(val), VAR_SAMP(val), STDDEV_POP(val), STDDEV_SAMP(val) FROM t3038_f;",
                    expected: Expected::Rows {
                        columns: &[Column("var_pop", FLOAT8), Column("var_samp", FLOAT8), Column("stddev_pop", FLOAT8), Column("stddev_samp", FLOAT8)],
                        rows: &[
                            &[T("25"), T("50"), T("5"), T("7.0710678118654755")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT VARIANCE(val), STDDEV(val) FROM t3038b WHERE grp = 'a';",
                    expected: Expected::Rows {
                        columns: &[Column("variance", NUMERIC), Column("stddev", NUMERIC)],
                        rows: &[
                            &[T("50.0000000000000000"), T("7.0710678118654752")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "variance/stddev over float8 avoid cancellation for large nearly-equal values",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT avg(x::float8), var_pop(x::float8), var_samp(x::float8), stddev_pop(x::float8), stddev_samp(x::float8) FROM (VALUES (100000003), (100000004), (100000006), (100000007)) v(x);",
                    expected: Expected::Rows {
                        columns: &[Column("avg", FLOAT8), Column("var_pop", FLOAT8), Column("var_samp", FLOAT8), Column("stddev_pop", FLOAT8), Column("stddev_samp", FLOAT8)],
                        rows: &[
                            &[T("100000005"), T("2.5"), T("3.3333333333333335"), T("1.5811388300841898"), T("1.8257418583505538")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT avg(x::float8), var_pop(x::float8), var_samp(x::float8), stddev_pop(x::float8), stddev_samp(x::float8) FROM (VALUES (7000000000005), (7000000000007)) v(x);",
                    expected: Expected::Rows {
                        columns: &[Column("avg", FLOAT8), Column("var_pop", FLOAT8), Column("var_samp", FLOAT8), Column("stddev_pop", FLOAT8), Column("stddev_samp", FLOAT8)],
                        rows: &[
                            &[T("7000000000006"), T("1"), T("2"), T("1"), T("1.4142135623730951")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "variance/stddev over a real column, as GROUP BY aggregates and window functions",
            set_up_script: &[
                "CREATE TABLE t3038r (id INT PRIMARY KEY, grp VARCHAR(10), val REAL);",
                "INSERT INTO t3038r VALUES (1,'a',10), (2,'a',20), (3,'b',30), (4,'b',5), (5,'c',15), (6,'c',25);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT grp, VAR_POP(val), VAR_SAMP(val), STDDEV_POP(val), STDDEV_SAMP(val) FROM t3038r GROUP BY grp ORDER BY grp;",
                    expected: Expected::Rows {
                        columns: &[Column("grp", VARCHAR), Column("var_pop", FLOAT8), Column("var_samp", FLOAT8), Column("stddev_pop", FLOAT8), Column("stddev_samp", FLOAT8)],
                        rows: &[
                            &[T("a"), T("25"), T("50"), T("5"), T("7.0710678118654755")],
                            &[T("b"), T("156.25"), T("312.5"), T("12.5"), T("17.67766952966369")],
                            &[T("c"), T("25"), T("50"), T("5"), T("7.0710678118654755")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, VAR_POP(val) OVER (ORDER BY grp) FROM t3038r ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("var_pop", FLOAT8)],
                        rows: &[
                            &[T("1"), T("25")],
                            &[T("2"), T("25")],
                            &[T("3"), T("92.1875")],
                            &[T("4"), T("92.1875")],
                            &[T("5"), T("72.91666666666667")],
                            &[T("6"), T("72.91666666666667")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a star argument is not a value",
            set_up_script: &[
                "CREATE TABLE tstar (id INT PRIMARY KEY, val INT);",
                "INSERT INTO tstar VALUES (1, 10), (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT FIRST_VALUE(*) OVER (ORDER BY id) FROM tstar;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function first_value() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) OVER (ORDER BY id) FROM tstar;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
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
fn test_range_offset_and_window_chain_rules() {
    run_scripts(&[
        ScriptTest {
            name: "RANGE frames with offsets",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE rg (d date, n int, f float8, t text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rg VALUES ('2020-01-31', 1, 1.5, 'a'), ('2020-02-29', 2, 2.5, 'b'), ('2020-03-31', 4, NULL, 'c'), (NULL, NULL, 4.0, 'd'), ('2020-03-01', 7, 7.5, 'e');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d, n, sum(n) OVER (ORDER BY d RANGE BETWEEN INTERVAL '1 month' PRECEDING AND CURRENT ROW) FROM rg ORDER BY d;",
                    expected: Expected::Rows {
                        columns: &[Column("d", DATE), Column("n", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("2020-01-31"), T("1"), T("1")],
                            &[T("2020-02-29"), T("2"), T("3")],
                            &[T("2020-03-01"), T("7"), T("9")],
                            &[T("2020-03-31"), T("4"), T("13")],
                            &[Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, sum(n) OVER (ORDER BY n RANGE BETWEEN 2 PRECEDING AND 1 FOLLOWING), sum(n) OVER (ORDER BY n DESC RANGE BETWEEN 2 PRECEDING AND 1 FOLLOWING) FROM rg ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("3"), T("3")],
                            &[T("2"), T("3"), T("7")],
                            &[T("4"), T("6"), T("4")],
                            &[T("7"), T("7"), T("7")],
                            &[Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f, sum(n) OVER (ORDER BY f RANGE BETWEEN 1.5 PRECEDING AND 1 FOLLOWING) FROM rg ORDER BY f;",
                    expected: Expected::Rows {
                        columns: &[Column("f", FLOAT8), Column("sum", INT8)],
                        rows: &[
                            &[T("1.5"), T("3")],
                            &[T("2.5"), T("3")],
                            &[T("4"), T("2")],
                            &[T("7.5"), T("7")],
                            &[Null, T("4")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, sum(n) OVER (ORDER BY n NULLS FIRST RANGE BETWEEN 1 FOLLOWING AND 3 FOLLOWING) FROM rg ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("6")],
                            &[T("2"), T("4")],
                            &[T("4"), T("7")],
                            &[T("7"), Null],
                            &[Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(n) OVER (ORDER BY n, f RANGE 1 PRECEDING) FROM rg;",
                    expected: Expected::Error(Diagnostic { code: "42P20", message: "RANGE with offset PRECEDING/FOLLOWING requires exactly one ORDER BY column", position: 20, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(n) OVER (ORDER BY t RANGE 1 PRECEDING) FROM rg;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "RANGE with offset PRECEDING/FOLLOWING is not supported for column type text", position: 38, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(n) OVER (ORDER BY n RANGE -1 PRECEDING) FROM rg;",
                    expected: Expected::Error(Diagnostic { code: "22013", message: "invalid preceding or following size in window function", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(n) OVER (ORDER BY n RANGE NULL PRECEDING) FROM rg;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "frame starting offset must not be null", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(n) OVER (ORDER BY d RANGE 1 PRECEDING) FROM rg;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "RANGE with offset PRECEDING/FOLLOWING is not supported for column type date and offset type integer", hint: "Cast the offset value to an appropriate type.", position: 38, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(n) OVER (RANGE 1 PRECEDING) FROM rg;",
                    expected: Expected::Error(Diagnostic { code: "42P20", message: "RANGE with offset PRECEDING/FOLLOWING requires exactly one ORDER BY column", position: 20, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "named window chains",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE nw (id int, grp int, amt int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO nw VALUES (1, 1, 10), (2, 1, 20), (3, 1, 30), (4, 2, 5), (5, 2, 15);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(amt) OVER w3 FROM nw WINDOW w1 AS (PARTITION BY grp), w2 AS (w1 ORDER BY id), w3 AS (w2) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("30")],
                            &[T("3"), T("60")],
                            &[T("4"), T("5")],
                            &[T("5"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(amt) OVER (w2 ROWS 1 PRECEDING) FROM nw WINDOW w1 AS (PARTITION BY grp), w2 AS (w1 ORDER BY id) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("30")],
                            &[T("3"), T("50")],
                            &[T("4"), T("5")],
                            &[T("5"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_running_window_frames() {
    run_scripts(&[
        ScriptTest {
            name: "running and moving window frames",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE wf (id INT PRIMARY KEY, g INT, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wf VALUES (1, 1, 5), (2, 1, 5), (3, 1, NULL), (4, 1, 7), (5, 2, 1), (6, 2, 1), (7, 2, 2), (8, 2, 3), (9, 3, NULL);",
                    expected: Expected::Tag("INSERT 0 9"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(v) OVER (ORDER BY v), count(*) OVER (ORDER BY v), max(v) OVER (ORDER BY v) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("count", INT8), Column("max", INT4)],
                        rows: &[
                            &[T("1"), T("17"), T("6"), T("5")],
                            &[T("2"), T("17"), T("6"), T("5")],
                            &[T("3"), T("24"), T("9"), T("7")],
                            &[T("4"), T("24"), T("7"), T("7")],
                            &[T("5"), T("2"), T("2"), T("1")],
                            &[T("6"), T("2"), T("2"), T("1")],
                            &[T("7"), T("4"), T("3"), T("2")],
                            &[T("8"), T("7"), T("4"), T("3")],
                            &[T("9"), T("24"), T("9"), T("7")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(v) OVER (PARTITION BY g ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW), avg(v) OVER (PARTITION BY g ORDER BY v) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("avg", NUMERIC)],
                        rows: &[
                            &[T("1"), T("5"), T("5.0000000000000000")],
                            &[T("2"), T("10"), T("5.0000000000000000")],
                            &[T("3"), T("10"), T("5.6666666666666667")],
                            &[T("4"), T("17"), T("5.6666666666666667")],
                            &[T("5"), T("1"), T("1.00000000000000000000")],
                            &[T("6"), T("2"), T("1.00000000000000000000")],
                            &[T("7"), T("4"), T("1.3333333333333333")],
                            &[T("8"), T("7"), T("1.7500000000000000")],
                            &[T("9"), Null, Null],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(v) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), min(v) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND 2 FOLLOWING) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("min", INT4)],
                        rows: &[
                            &[T("1"), Null, T("5")],
                            &[T("2"), T("5"), T("5")],
                            &[T("3"), T("10"), T("1")],
                            &[T("4"), T("10"), T("1")],
                            &[T("5"), T("17"), T("1")],
                            &[T("6"), T("18"), T("1")],
                            &[T("7"), T("19"), T("1")],
                            &[T("8"), T("21"), T("1")],
                            &[T("9"), T("24"), T("1")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(v) OVER (ORDER BY v GROUPS BETWEEN UNBOUNDED PRECEDING AND 1 FOLLOWING), count(v) OVER (ORDER BY v RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("24"), T("7")],
                            &[T("2"), T("24"), T("7")],
                            &[T("3"), T("24"), T("7")],
                            &[T("4"), T("24"), T("7")],
                            &[T("5"), T("4"), T("7")],
                            &[T("6"), T("4"), T("7")],
                            &[T("7"), T("7"), T("7")],
                            &[T("8"), T("17"), T("7")],
                            &[T("9"), T("24"), T("7")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(v) OVER (ORDER BY v, id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW), sum(v) OVER (ORDER BY v RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE TIES) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("7"), T("12")],
                            &[T("2"), T("12"), T("12")],
                            &[T("3"), T("24"), T("24")],
                            &[T("4"), T("17"), T("24")],
                            &[T("5"), Null, T("1")],
                            &[T("6"), T("1"), T("1")],
                            &[T("7"), T("2"), T("4")],
                            &[T("8"), T("4"), T("7")],
                            &[T("9"), T("24"), T("24")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, string_agg(v::text, ',') OVER (ORDER BY id), array_agg(v) OVER (PARTITION BY g ORDER BY v DESC) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("string_agg", TEXT), Column("array_agg", INT4_ARRAY)],
                        rows: &[
                            &[T("1"), T("5"), T("{NULL,7,5,5}")],
                            &[T("2"), T("5,5"), T("{NULL,7,5,5}")],
                            &[T("3"), T("5,5"), T("{NULL}")],
                            &[T("4"), T("5,5,7"), T("{NULL,7}")],
                            &[T("5"), T("5,5,7,1"), T("{3,2,1,1}")],
                            &[T("6"), T("5,5,7,1,1"), T("{3,2,1,1}")],
                            &[T("7"), T("5,5,7,1,1,2"), T("{3,2}")],
                            &[T("8"), T("5,5,7,1,1,2,3"), T("{3}")],
                            &[T("9"), T("5,5,7,1,1,2,3"), T("{NULL}")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, rank() OVER (ORDER BY v), dense_rank() OVER (ORDER BY v), percent_rank() OVER (ORDER BY v), cume_dist() OVER (ORDER BY v) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("rank", INT8), Column("dense_rank", INT8), Column("percent_rank", FLOAT8), Column("cume_dist", FLOAT8)],
                        rows: &[
                            &[T("1"), T("5"), T("4"), T("0.5"), T("0.6666666666666666")],
                            &[T("2"), T("5"), T("4"), T("0.5"), T("0.6666666666666666")],
                            &[T("3"), T("8"), T("6"), T("0.875"), T("1")],
                            &[T("4"), T("7"), T("5"), T("0.75"), T("0.7777777777777778")],
                            &[T("5"), T("1"), T("1"), T("0"), T("0.2222222222222222")],
                            &[T("6"), T("1"), T("1"), T("0"), T("0.2222222222222222")],
                            &[T("7"), T("3"), T("2"), T("0.25"), T("0.3333333333333333")],
                            &[T("8"), T("4"), T("3"), T("0.375"), T("0.4444444444444444")],
                            &[T("9"), T("8"), T("6"), T("0.875"), T("1")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_value(id) OVER (ORDER BY v, id), last_value(id) OVER (ORDER BY v, id), nth_value(id, 2) OVER (ORDER BY v, id) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("first_value", INT4), Column("last_value", INT4), Column("nth_value", INT4)],
                        rows: &[
                            &[T("1"), T("5"), T("1"), T("6")],
                            &[T("2"), T("5"), T("2"), T("6")],
                            &[T("3"), T("5"), T("3"), T("6")],
                            &[T("4"), T("5"), T("4"), T("6")],
                            &[T("5"), T("5"), T("5"), Null],
                            &[T("6"), T("5"), T("6"), T("6")],
                            &[T("7"), T("5"), T("7"), T("6")],
                            &[T("8"), T("5"), T("8"), T("6")],
                            &[T("9"), T("5"), T("9"), T("6")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(v) OVER (ORDER BY v RANGE BETWEEN 1 PRECEDING AND CURRENT ROW), bool_and(v > 1) OVER (ORDER BY id) FROM wf ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("bool_and", BOOL)],
                        rows: &[
                            &[T("1"), T("10"), T("t")],
                            &[T("2"), T("10"), T("t")],
                            &[T("3"), Null, T("t")],
                            &[T("4"), T("7"), T("t")],
                            &[T("5"), T("2"), T("f")],
                            &[T("6"), T("2"), T("f")],
                            &[T("7"), T("4"), T("f")],
                            &[T("8"), T("5"), T("f")],
                            &[T("9"), Null, T("f")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_moving_window_frames() {
    run_scripts(&[
        ScriptTest {
            name: "moving window frames",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mw (id INT PRIMARY KEY, g INT, i INT, n NUMERIC);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mw VALUES (1, 1, 5, 1.5), (2, 1, NULL, 2.25), (3, 1, 7, NULL), (4, 1, 2, 3), (5, 1, 9, 0.125), (6, 2, 1, 10), (7, 2, 4, 'NaN'), (8, 2, NULL, 1.0), (9, 2, 6, 2.50), (10, 2, 3, 4), (11, 2, 8, 0.5), (12, 1, 1, 7.75);",
                    expected: Expected::Tag("INSERT 0 12"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(i) OVER w, count(i) OVER w, count(*) OVER w, avg(i) OVER w FROM mw WINDOW w AS (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 1 FOLLOWING) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("count", INT8), Column("count", INT8), Column("avg", NUMERIC)],
                        rows: &[
                            &[T("1"), T("5"), T("1"), T("2"), T("5.0000000000000000")],
                            &[T("2"), T("12"), T("2"), T("3"), T("6.0000000000000000")],
                            &[T("3"), T("14"), T("3"), T("4"), T("4.6666666666666667")],
                            &[T("4"), T("18"), T("3"), T("4"), T("6.0000000000000000")],
                            &[T("5"), T("19"), T("4"), T("4"), T("4.7500000000000000")],
                            &[T("6"), T("16"), T("4"), T("4"), T("4.0000000000000000")],
                            &[T("7"), T("14"), T("3"), T("4"), T("4.6666666666666667")],
                            &[T("8"), T("11"), T("3"), T("4"), T("3.6666666666666667")],
                            &[T("9"), T("13"), T("3"), T("4"), T("4.3333333333333333")],
                            &[T("10"), T("17"), T("3"), T("4"), T("5.6666666666666667")],
                            &[T("11"), T("18"), T("4"), T("4"), T("4.5000000000000000")],
                            &[T("12"), T("12"), T("3"), T("3"), T("4.0000000000000000")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(n) OVER w, avg(n) OVER w, count(n) OVER w FROM mw WINDOW w AS (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", NUMERIC), Column("avg", NUMERIC), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("3.75"), T("1.8750000000000000"), T("2")],
                            &[T("2"), T("3.75"), T("1.8750000000000000"), T("2")],
                            &[T("3"), T("5.25"), T("2.6250000000000000"), T("2")],
                            &[T("4"), T("3.125"), T("1.5625000000000000"), T("2")],
                            &[T("5"), T("13.125"), T("4.3750000000000000"), T("3")],
                            &[T("6"), T("NaN"), T("NaN"), T("3")],
                            &[T("7"), T("NaN"), T("NaN"), T("3")],
                            &[T("8"), T("NaN"), T("NaN"), T("3")],
                            &[T("9"), T("7.50"), T("2.5000000000000000"), T("3")],
                            &[T("10"), T("7.00"), T("2.3333333333333333"), T("3")],
                            &[T("11"), T("12.25"), T("4.0833333333333333"), T("3")],
                            &[T("12"), T("8.25"), T("4.1250000000000000"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(n) OVER w, avg(n) OVER w FROM mw WINDOW w AS (PARTITION BY g ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", NUMERIC), Column("avg", NUMERIC)],
                        rows: &[
                            &[T("1"), T("1.5"), T("1.50000000000000000000")],
                            &[T("2"), T("3.75"), T("1.8750000000000000")],
                            &[T("3"), T("2.25"), T("2.2500000000000000")],
                            &[T("4"), T("3"), T("3.0000000000000000")],
                            &[T("5"), T("3.125"), T("1.5625000000000000")],
                            &[T("6"), T("10"), T("10.0000000000000000")],
                            &[T("7"), T("NaN"), T("NaN")],
                            &[T("8"), T("NaN"), T("NaN")],
                            &[T("9"), T("3.50"), T("1.7500000000000000")],
                            &[T("10"), T("6.50"), T("3.2500000000000000")],
                            &[T("11"), T("4.5"), T("2.2500000000000000")],
                            &[T("12"), T("7.875"), T("3.9375000000000000")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(i) FILTER (WHERE i > 3) OVER w, count(*) FILTER (WHERE n > 1) OVER w FROM mw WINDOW w AS (ORDER BY id ROWS BETWEEN 3 PRECEDING AND 1 PRECEDING) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("1"), Null, T("0")],
                            &[T("2"), T("5"), T("1")],
                            &[T("3"), T("5"), T("2")],
                            &[T("4"), T("12"), T("2")],
                            &[T("5"), T("7"), T("2")],
                            &[T("6"), T("16"), T("1")],
                            &[T("7"), T("9"), T("2")],
                            &[T("8"), T("13"), T("2")],
                            &[T("9"), T("4"), T("2")],
                            &[T("10"), T("10"), T("2")],
                            &[T("11"), T("6"), T("2")],
                            &[T("12"), T("14"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(i) OVER w, avg(n) OVER w FROM mw WINDOW w AS (ORDER BY id ROWS BETWEEN 2 FOLLOWING AND 4 FOLLOWING) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("avg", NUMERIC)],
                        rows: &[
                            &[T("1"), T("18"), T("1.5625000000000000")],
                            &[T("2"), T("12"), T("4.3750000000000000")],
                            &[T("3"), T("14"), T("NaN")],
                            &[T("4"), T("5"), T("NaN")],
                            &[T("5"), T("10"), T("NaN")],
                            &[T("6"), T("9"), T("2.5000000000000000")],
                            &[T("7"), T("17"), T("2.3333333333333333")],
                            &[T("8"), T("12"), T("4.0833333333333333")],
                            &[T("9"), T("9"), T("4.1250000000000000")],
                            &[T("10"), T("1"), T("7.7500000000000000")],
                            &[T("11"), Null, Null],
                            &[T("12"), Null, Null],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(i) OVER w, count(*) OVER w FROM mw WINDOW w AS (ORDER BY i RANGE BETWEEN 2 PRECEDING AND 1 FOLLOWING) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("18"), T("4")],
                            &[T("2"), Null, T("2")],
                            &[T("3"), T("26"), T("4")],
                            &[T("4"), T("7"), T("4")],
                            &[T("5"), T("24"), T("3")],
                            &[T("6"), T("4"), T("3")],
                            &[T("7"), T("14"), T("4")],
                            &[T("8"), Null, T("2")],
                            &[T("9"), T("22"), T("4")],
                            &[T("10"), T("11"), T("5")],
                            &[T("11"), T("30"), T("4")],
                            &[T("12"), T("4"), T("3")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(n) OVER w FROM mw WINDOW w AS (ORDER BY g GROUPS BETWEEN CURRENT ROW AND 1 FOLLOWING) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", NUMERIC)],
                        rows: &[
                            &[T("1"), T("NaN")],
                            &[T("2"), T("NaN")],
                            &[T("3"), T("NaN")],
                            &[T("4"), T("NaN")],
                            &[T("5"), T("NaN")],
                            &[T("6"), T("NaN")],
                            &[T("7"), T("NaN")],
                            &[T("8"), T("NaN")],
                            &[T("9"), T("NaN")],
                            &[T("10"), T("NaN")],
                            &[T("11"), T("NaN")],
                            &[T("12"), T("NaN")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, avg(i::int8) OVER w, sum(i::int2) OVER w FROM mw WINDOW w AS (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 2 FOLLOWING) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("avg", NUMERIC), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("6.0000000000000000"), T("12")],
                            &[T("2"), T("4.6666666666666667"), T("14")],
                            &[T("3"), T("6.0000000000000000"), T("18")],
                            &[T("4"), T("4.7500000000000000"), T("19")],
                            &[T("5"), T("4.0000000000000000"), T("16")],
                            &[T("6"), T("4.6666666666666667"), T("14")],
                            &[T("7"), T("3.6666666666666667"), T("11")],
                            &[T("8"), T("4.3333333333333333"), T("13")],
                            &[T("9"), T("5.6666666666666667"), T("17")],
                            &[T("10"), T("4.5000000000000000"), T("18")],
                            &[T("11"), T("4.0000000000000000"), T("12")],
                            &[T("12"), T("4.5000000000000000"), T("9")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_window_sort_order() {
    run_scripts(&[
        ScriptTest {
            name: "Window rows come out in the order of the last window's sort",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE wo (id INT PRIMARY KEY, p INT, o INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES (1, 2, 1), (2, 1, 2), (3, 2, 0), (4, 1, 1), (5, 3, 5);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, p, o, row_number() OVER (PARTITION BY p ORDER BY o) FROM wo;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("p", INT4), Column("o", INT4), Column("row_number", INT8)],
                        rows: &[
                            &[T("4"), T("1"), T("1"), T("1")],
                            &[T("2"), T("1"), T("2"), T("2")],
                            &[T("3"), T("2"), T("0"), T("1")],
                            &[T("1"), T("2"), T("1"), T("2")],
                            &[T("5"), T("3"), T("5"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(o) OVER (PARTITION BY p), rank() OVER (ORDER BY o DESC) FROM wo;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("rank", INT8)],
                        rows: &[
                            &[T("2"), T("3"), T("2")],
                            &[T("4"), T("3"), T("3")],
                            &[T("1"), T("1"), T("3")],
                            &[T("3"), T("1"), T("5")],
                            &[T("5"), T("5"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, rank() OVER (ORDER BY o DESC), sum(o) OVER (PARTITION BY p) FROM wo;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("rank", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("5"), T("1"), T("5")],
                            &[T("2"), T("2"), T("3")],
                            &[T("4"), T("3"), T("3")],
                            &[T("1"), T("3"), T("1")],
                            &[T("3"), T("5"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, count(*) OVER (PARTITION BY p ORDER BY o), count(*) OVER (PARTITION BY p) FROM wo;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("4"), T("1"), T("2")],
                            &[T("2"), T("2"), T("2")],
                            &[T("3"), T("1"), T("2")],
                            &[T("1"), T("2"), T("2")],
                            &[T("5"), T("1"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, count(*) OVER () FROM wo WHERE id > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("4"), T("3")],
                            &[T("5"), T("3")],
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
fn test_window_agg() {
    run_scripts(&[
        ScriptTest {
            name: "WindowAgg's window functions, frames, moving aggregates, and plans",
            set_up_script: &[
                "CREATE TABLE ws (id int PRIMARY KEY, dep text, sal int, d date, n numeric, iv interval, f boolean) WITH (autovacuum_enabled = false);",
                "INSERT INTO ws VALUES (1, 'a', 100, '2024-01-01', 1.5, '1 day', true), (2, 'a', 200, '2024-01-03', 2.25, '2 days', true), (3, 'a', 200, '2024-02-01', NULL, '3 hours', false), (4, 'b', 50, '2024-01-02', 10, NULL, NULL), (5, 'b', NULL, '2024-03-01', 0.125, '1 month', true), (6, 'b', 75, '2024-03-02', 3, '5 days', false), (7, 'c', 300, NULL, 7.75, '10 days', true);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, dep, sal, row_number() OVER w, rank() OVER w, dense_rank() OVER w, percent_rank() OVER w, cume_dist() OVER w FROM ws WINDOW w AS (PARTITION BY dep ORDER BY sal) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("dep", TEXT), Column("sal", INT4), Column("row_number", INT8), Column("rank", INT8), Column("dense_rank", INT8), Column("percent_rank", FLOAT8), Column("cume_dist", FLOAT8)],
                        rows: &[
                            &[T("1"), T("a"), T("100"), T("1"), T("1"), T("1"), T("0"), T("0.3333333333333333")],
                            &[T("2"), T("a"), T("200"), T("2"), T("2"), T("2"), T("0.5"), T("1")],
                            &[T("3"), T("a"), T("200"), T("3"), T("2"), T("2"), T("0.5"), T("1")],
                            &[T("4"), T("b"), T("50"), T("1"), T("1"), T("1"), T("0"), T("0.3333333333333333")],
                            &[T("5"), T("b"), Null, T("3"), T("3"), T("3"), T("1"), T("1")],
                            &[T("6"), T("b"), T("75"), T("2"), T("2"), T("2"), T("0.5"), T("0.6666666666666666")],
                            &[T("7"), T("c"), T("300"), T("1"), T("1"), T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, ntile(3) OVER (ORDER BY id), ntile(10) OVER (ORDER BY id) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ntile", INT4), Column("ntile", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("1"), T("2")],
                            &[T("3"), T("1"), T("3")],
                            &[T("4"), T("2"), T("4")],
                            &[T("5"), T("2"), T("5")],
                            &[T("6"), T("3"), T("6")],
                            &[T("7"), T("3"), T("7")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ntile(0) OVER (ORDER BY id) FROM ws;",
                    expected: Expected::Error(Diagnostic { code: "22014", message: "argument of ntile must be greater than zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, ntile(NULL) OVER (ORDER BY id) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ntile", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, lag(sal) OVER w, lag(sal, 2) OVER w, lag(sal, 1, -1) OVER w, lead(sal) OVER w, lead(sal, 3) OVER w, lead(sal, 1, 0) OVER w FROM ws WINDOW w AS (ORDER BY id) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("lag", INT4), Column("lag", INT4), Column("lag", INT4), Column("lead", INT4), Column("lead", INT4), Column("lead", INT4)],
                        rows: &[
                            &[T("1"), Null, Null, T("-1"), T("200"), T("50"), T("200")],
                            &[T("2"), T("100"), Null, T("100"), T("200"), Null, T("200")],
                            &[T("3"), T("200"), T("100"), T("200"), T("50"), T("75"), T("50")],
                            &[T("4"), T("200"), T("200"), T("200"), Null, T("300"), Null],
                            &[T("5"), T("50"), T("200"), T("50"), T("75"), Null, T("75")],
                            &[T("6"), Null, T("50"), Null, T("300"), Null, T("300")],
                            &[T("7"), T("75"), Null, T("75"), Null, Null, T("0")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, lag(sal, -1) OVER (ORDER BY id), lead(sal, NULL) OVER (ORDER BY id) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("lag", INT4), Column("lead", INT4)],
                        rows: &[
                            &[T("1"), T("200"), Null],
                            &[T("2"), T("200"), Null],
                            &[T("3"), T("50"), Null],
                            &[T("4"), Null, Null],
                            &[T("5"), T("75"), Null],
                            &[T("6"), T("300"), Null],
                            &[T("7"), Null, Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_value(sal) OVER w, last_value(sal) OVER w, nth_value(sal, 2) OVER w, nth_value(sal, 5) OVER w FROM ws WINDOW w AS (PARTITION BY dep ORDER BY id) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("first_value", INT4), Column("last_value", INT4), Column("nth_value", INT4), Column("nth_value", INT4)],
                        rows: &[
                            &[T("1"), T("100"), T("100"), Null, Null],
                            &[T("2"), T("100"), T("200"), T("200"), Null],
                            &[T("3"), T("100"), T("200"), T("200"), Null],
                            &[T("4"), T("50"), T("50"), Null, Null],
                            &[T("5"), T("50"), Null, Null, Null],
                            &[T("6"), T("50"), T("75"), Null, Null],
                            &[T("7"), T("300"), T("300"), Null, Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nth_value(sal, 0) OVER (ORDER BY id) FROM ws;",
                    expected: Expected::Error(Diagnostic { code: "22016", message: "argument of nth_value must be greater than zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(sal) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING), count(*) OVER (ORDER BY id ROWS BETWEEN 2 FOLLOWING AND 3 FOLLOWING), max(sal) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND 2 PRECEDING) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("count", INT8), Column("max", INT4)],
                        rows: &[
                            &[T("1"), T("300"), T("2"), Null],
                            &[T("2"), T("500"), T("2"), Null],
                            &[T("3"), T("450"), T("2"), T("100")],
                            &[T("4"), T("250"), T("2"), T("200")],
                            &[T("5"), T("125"), T("1"), T("200")],
                            &[T("6"), T("375"), T("0"), T("200")],
                            &[T("7"), T("375"), T("0"), T("200")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sal, sum(id) OVER (ORDER BY sal RANGE BETWEEN 50 PRECEDING AND 50 FOLLOWING), sum(id) OVER (ORDER BY sal DESC NULLS LAST RANGE BETWEEN 100 PRECEDING AND CURRENT ROW) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sal", INT4), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("100"), T("11"), T("6")],
                            &[T("2"), T("200"), T("5"), T("12")],
                            &[T("3"), T("200"), T("5"), T("12")],
                            &[T("4"), T("50"), T("11"), T("11")],
                            &[T("5"), Null, T("5"), T("5")],
                            &[T("6"), T("75"), T("11"), T("7")],
                            &[T("7"), T("300"), T("7"), T("7")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, count(*) OVER (ORDER BY d RANGE BETWEEN '7 days' PRECEDING AND '1 month' FOLLOWING), string_agg(id::text, ',') OVER (ORDER BY d RANGE BETWEEN CURRENT ROW AND '2 days' FOLLOWING) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", DATE), Column("count", INT8), Column("string_agg", TEXT)],
                        rows: &[
                            &[T("1"), T("2024-01-01"), T("4"), T("1,4,2")],
                            &[T("2"), T("2024-01-03"), T("4"), T("2")],
                            &[T("3"), T("2024-02-01"), T("2"), T("3")],
                            &[T("4"), T("2024-01-02"), T("4"), T("4,2")],
                            &[T("5"), T("2024-03-01"), T("2"), T("5,6")],
                            &[T("6"), T("2024-03-02"), T("2"), T("6")],
                            &[T("7"), Null, T("1"), T("7")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, n, sum(id) OVER (ORDER BY n RANGE BETWEEN 1.5 PRECEDING AND 0.5 FOLLOWING) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("n", NUMERIC), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("1.5"), T("6")],
                            &[T("2"), T("2.25"), T("3")],
                            &[T("3"), Null, T("3")],
                            &[T("4"), T("10"), T("4")],
                            &[T("5"), T("0.125"), T("5")],
                            &[T("6"), T("3"), T("9")],
                            &[T("7"), T("7.75"), T("7")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sal, sum(id) OVER (ORDER BY sal GROUPS BETWEEN 1 PRECEDING AND 1 FOLLOWING), first_value(id) OVER (ORDER BY sal GROUPS BETWEEN CURRENT ROW AND 2 FOLLOWING) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sal", INT4), Column("sum", INT8), Column("first_value", INT4)],
                        rows: &[
                            &[T("1"), T("100"), T("12"), T("1")],
                            &[T("2"), T("200"), T("13"), T("2")],
                            &[T("3"), T("200"), T("13"), T("2")],
                            &[T("4"), T("50"), T("10"), T("4")],
                            &[T("5"), Null, T("12"), T("5")],
                            &[T("6"), T("75"), T("11"), T("6")],
                            &[T("7"), T("300"), T("17"), T("7")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sal, sum(id) OVER (ORDER BY sal ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING EXCLUDE CURRENT ROW), sum(id) OVER (ORDER BY sal ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING EXCLUDE GROUP), sum(id) OVER (ORDER BY sal ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING EXCLUDE TIES) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sal", INT4), Column("sum", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("1"), T("100"), T("15"), T("15"), T("16")],
                            &[T("2"), T("200"), T("17"), T("14"), T("16")],
                            &[T("3"), T("200"), T("15"), T("13"), T("16")],
                            &[T("4"), T("50"), T("7"), T("7"), T("11")],
                            &[T("5"), Null, T("10"), T("10"), T("15")],
                            &[T("6"), T("75"), T("7"), T("7"), T("13")],
                            &[T("7"), T("300"), T("10"), T("10"), T("17")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sal, first_value(id) OVER w, last_value(id) OVER w, nth_value(id, 2) OVER w FROM ws WINDOW w AS (ORDER BY sal RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE TIES) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sal", INT4), Column("first_value", INT4), Column("last_value", INT4), Column("nth_value", INT4)],
                        rows: &[
                            &[T("1"), T("100"), T("4"), T("5"), T("6")],
                            &[T("2"), T("200"), T("4"), T("5"), T("6")],
                            &[T("3"), T("200"), T("4"), T("5"), T("6")],
                            &[T("4"), T("50"), T("4"), T("5"), T("6")],
                            &[T("5"), Null, T("4"), T("5"), T("6")],
                            &[T("6"), T("75"), T("4"), T("5"), T("6")],
                            &[T("7"), T("300"), T("4"), T("5"), T("6")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(sal) OVER (ORDER BY id ROWS BETWEEN -1 PRECEDING AND CURRENT ROW) FROM ws;",
                    expected: Expected::Error(Diagnostic { code: "22013", message: "frame starting offset must not be negative", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(sal) OVER (ORDER BY id ROWS BETWEEN NULL PRECEDING AND CURRENT ROW) FROM ws;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "frame starting offset must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(sal) OVER (ORDER BY sal RANGE BETWEEN -10 PRECEDING AND CURRENT ROW) FROM ws;",
                    expected: Expected::Error(Diagnostic { code: "22013", message: "invalid preceding or following size in window function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(sal) OVER w, avg(sal) OVER w, count(sal) OVER w, min(sal) OVER w, stddev_samp(sal) OVER w FROM ws WINDOW w AS (ORDER BY id ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("avg", NUMERIC), Column("count", INT8), Column("min", INT4), Column("stddev_samp", NUMERIC)],
                        rows: &[
                            &[T("1"), T("100"), T("100.0000000000000000"), T("1"), T("100"), Null],
                            &[T("2"), T("300"), T("150.0000000000000000"), T("2"), T("100"), T("70.7106781186547524")],
                            &[T("3"), T("500"), T("166.6666666666666667"), T("3"), T("100"), T("57.7350269189625765")],
                            &[T("4"), T("450"), T("150.0000000000000000"), T("3"), T("50"), T("86.6025403784438647")],
                            &[T("5"), T("250"), T("125.0000000000000000"), T("2"), T("50"), T("106.0660171779821287")],
                            &[T("6"), T("125"), T("62.5000000000000000"), T("2"), T("50"), T("17.6776695296636881")],
                            &[T("7"), T("375"), T("187.5000000000000000"), T("2"), T("75"), T("159.099025766973")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(sal::bigint) OVER w, avg(sal::smallint) OVER w, var_pop(sal::bigint) OVER w FROM ws WINDOW w AS (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", NUMERIC), Column("avg", NUMERIC), Column("var_pop", NUMERIC)],
                        rows: &[
                            &[T("1"), T("300"), T("150.0000000000000000"), T("2500.0000000000000000")],
                            &[T("2"), T("500"), T("166.6666666666666667"), T("2222.2222222222222222")],
                            &[T("3"), T("450"), T("150.0000000000000000"), T("5000.0000000000000000")],
                            &[T("4"), T("250"), T("125.0000000000000000"), T("5625.0000000000000000")],
                            &[T("5"), T("125"), T("62.5000000000000000"), T("156.2500000000000000")],
                            &[T("6"), T("375"), T("187.5000000000000000"), T("12656.250000000000")],
                            &[T("7"), T("375"), T("187.5000000000000000"), T("12656.250000000000")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(n) OVER w, avg(n) OVER w, var_samp(n) OVER w FROM ws WINDOW w AS (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", NUMERIC), Column("avg", NUMERIC), Column("var_samp", NUMERIC)],
                        rows: &[
                            &[T("1"), T("1.5"), T("1.50000000000000000000"), Null],
                            &[T("2"), T("3.75"), T("1.8750000000000000"), T("0.28125000000000000000")],
                            &[T("3"), T("2.25"), T("2.2500000000000000"), Null],
                            &[T("4"), T("10"), T("10.0000000000000000"), Null],
                            &[T("5"), T("10.125"), T("5.0625000000000000"), T("48.7578125000000000")],
                            &[T("6"), T("3.125"), T("1.5625000000000000"), T("4.1328125000000000")],
                            &[T("7"), T("10.75"), T("5.3750000000000000"), T("11.2812500000000000")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, bool_and(f) OVER w, bool_or(f) OVER w, every(f) OVER w FROM ws WINDOW w AS (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("bool_and", BOOL), Column("bool_or", BOOL), Column("every", BOOL)],
                        rows: &[
                            &[T("1"), T("t"), T("t"), T("t")],
                            &[T("2"), T("t"), T("t"), T("t")],
                            &[T("3"), T("f"), T("t"), T("f")],
                            &[T("4"), T("f"), T("f"), T("f")],
                            &[T("5"), T("t"), T("t"), T("t")],
                            &[T("6"), T("f"), T("t"), T("f")],
                            &[T("7"), T("f"), T("t"), T("f")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(iv) OVER w, avg(iv) OVER w FROM ws WINDOW w AS (ORDER BY id ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INTERVAL), Column("avg", INTERVAL)],
                        rows: &[
                            &[T("1"), T("1 day"), T("1 day")],
                            &[T("2"), T("3 days"), T("1 day 12:00:00")],
                            &[T("3"), T("3 days 03:00:00"), T("1 day 01:00:00")],
                            &[T("4"), T("2 days 03:00:00"), T("1 day 01:30:00")],
                            &[T("5"), T("1 mon 03:00:00"), T("15 days 01:30:00")],
                            &[T("6"), T("1 mon 5 days"), T("17 days 12:00:00")],
                            &[T("7"), T("1 mon 15 days"), T("15 days")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, sum(sal) FILTER (WHERE f) OVER (ORDER BY id), count(*) FILTER (WHERE sal > 100) OVER (PARTITION BY dep) FROM ws ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("100"), T("2")],
                            &[T("2"), T("300"), T("2")],
                            &[T("3"), T("300"), T("2")],
                            &[T("4"), T("300"), T("0")],
                            &[T("5"), T("300"), T("0")],
                            &[T("6"), T("300"), T("0")],
                            &[T("7"), T("600"), T("1")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dep, sum(sal), rank() OVER (ORDER BY sum(sal) DESC), sum(sum(sal)) OVER () FROM ws GROUP BY dep ORDER BY dep;",
                    expected: Expected::Rows {
                        columns: &[Column("dep", TEXT), Column("sum", INT8), Column("rank", INT8), Column("sum", NUMERIC)],
                        rows: &[
                            &[T("a"), T("500"), T("1"), T("925")],
                            &[T("b"), T("125"), T("3"), T("925")],
                            &[T("c"), T("300"), T("2"), T("925")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, row_number() OVER (PARTITION BY dep ORDER BY sal DESC) AS rn FROM ws) s WHERE rn = 1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("rn", INT8)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("5"), T("1")],
                            &[T("7"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, row_number() OVER (), count(*) OVER () FROM ws WHERE id > 100;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("row_number", INT8), Column("count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x, sum(x) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING), avg(x::numeric) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM (VALUES (1.0), (2.50), (3.125), (4)) v(x);",
                    expected: Expected::Rows {
                        columns: &[Column("x", NUMERIC), Column("sum", NUMERIC), Column("avg", NUMERIC)],
                        rows: &[
                            &[T("1.0"), T("3.50"), T("1.00000000000000000000")],
                            &[T("2.50"), T("6.625"), T("1.7500000000000000")],
                            &[T("3.125"), T("9.625"), T("2.8125000000000000")],
                            &[T("4"), T("7.125"), T("3.5625000000000000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id, row_number() OVER (PARTITION BY dep ORDER BY sal), sum(sal) OVER (PARTITION BY dep ORDER BY sal), rank() OVER (ORDER BY d) FROM ws;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w2 AS (PARTITION BY dep ORDER BY sal)")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: dep, sal")],
                            &[T("        ->  WindowAgg")],
                            &[T("              Window: w1 AS (ORDER BY d ROWS UNBOUNDED PRECEDING)")],
                            &[T("              ->  Sort")],
                            &[T("                    Sort Key: d")],
                            &[T("                    ->  Seq Scan on ws")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id, sum(sal) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 2 FOLLOWING EXCLUDE TIES), count(*) OVER (PARTITION BY dep) FROM ws;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w2 AS (ORDER BY id ROWS BETWEEN '1'::bigint PRECEDING AND '2'::bigint FOLLOWING EXCLUDE TIES)")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: id")],
                            &[T("        ->  WindowAgg")],
                            &[T("              Window: w1 AS (PARTITION BY dep)")],
                            &[T("              ->  Sort")],
                            &[T("                    Sort Key: dep")],
                            &[T("                    ->  Seq Scan on ws")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF, VERBOSE) SELECT id, lag(sal) OVER w, sum(sal) OVER (w RANGE BETWEEN 10 PRECEDING AND CURRENT ROW) FROM ws WINDOW w AS (ORDER BY sal);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Output: id, (lag(sal) OVER w), sum(sal) OVER w1, sal")],
                            &[T("  Window: w1 AS (ORDER BY ws.sal RANGE BETWEEN 10 PRECEDING AND CURRENT ROW)")],
                            &[T("  ->  WindowAgg")],
                            &[T("        Output: sal, id, lag(sal) OVER w")],
                            &[T("        Window: w AS (ORDER BY ws.sal)")],
                            &[T("        ->  Sort")],
                            &[T("              Output: sal, id")],
                            &[T("              Sort Key: ws.sal")],
                            &[T("              ->  Seq Scan on public.ws")],
                            &[T("                    Output: sal, id")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT row_number() OVER (ORDER BY d), rank() OVER (ORDER BY d RANGE UNBOUNDED PRECEDING) FROM ws;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w1 AS (ORDER BY d ROWS UNBOUNDED PRECEDING)")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: d")],
                            &[T("        ->  Seq Scan on ws")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT id, sum(sal) OVER (PARTITION BY dep) FROM ws;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg  (cost=48.18..59.87 rows=670 width=44)")],
                            &[T("  Window: w1 AS (PARTITION BY dep)")],
                            &[T("  ->  Sort  (cost=48.15..49.82 rows=670 width=40)")],
                            &[T("        Sort Key: dep")],
                            &[T("        ->  Seq Scan on ws  (cost=0.00..16.70 rows=670 width=40)")],
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
fn test_window_agg_run_conditions() {
    run_scripts(&[
        ScriptTest {
            name: "conditions on a subquery's monotonic window functions become WindowAgg run conditions",
            set_up_script: &[
                "CREATE TABLE wr (id int PRIMARY KEY, dep text, sal int) WITH (autovacuum_enabled = false);",
                "INSERT INTO wr VALUES (1, 'a', 100), (2, 'a', 200), (3, 'a', 200), (4, 'b', 50), (5, 'b', 75), (6, 'b', 75), (7, 'c', 300), (8, 'c', 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, sal, row_number() OVER (ORDER BY sal DESC) rn FROM wr) s WHERE rn < 4 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sal", INT4), Column("rn", INT8)],
                        rows: &[
                            &[T("2"), T("200"), T("2")],
                            &[T("3"), T("200"), T("3")],
                            &[T("7"), T("300"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT id, sal, row_number() OVER (ORDER BY sal DESC) rn FROM wr) s WHERE rn < 4;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w1 AS (ORDER BY wr.sal ROWS UNBOUNDED PRECEDING)")],
                            &[T("  Run Condition: (row_number() OVER w1 < 4)")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: wr.sal DESC")],
                            &[T("        ->  Seq Scan on wr")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, dep, sal, rank() OVER (PARTITION BY dep ORDER BY sal) r FROM wr) s WHERE r <= 1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("dep", TEXT), Column("sal", INT4), Column("r", INT8)],
                        rows: &[
                            &[T("1"), T("a"), T("100"), T("1")],
                            &[T("4"), T("b"), T("50"), T("1")],
                            &[T("8"), T("c"), T("10"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT id, dep, sal, rank() OVER (PARTITION BY dep ORDER BY sal) r FROM wr) s WHERE r <= 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w1 AS (PARTITION BY wr.dep ORDER BY wr.sal ROWS UNBOUNDED PRECEDING)")],
                            &[T("  Run Condition: (rank() OVER w1 <= 1)")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: wr.dep, wr.sal")],
                            &[T("        ->  Seq Scan on wr")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, sal, dense_rank() OVER (ORDER BY sal) dr FROM wr) s WHERE dr = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sal", INT4), Column("dr", INT8)],
                        rows: &[
                            &[T("4"), T("50"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT id, sal, dense_rank() OVER (ORDER BY sal) dr FROM wr) s WHERE dr = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Subquery Scan on s")],
                            &[T("  Filter: (s.dr = 2)")],
                            &[T("  ->  WindowAgg")],
                            &[T("        Window: w1 AS (ORDER BY wr.sal ROWS UNBOUNDED PRECEDING)")],
                            &[T("        Run Condition: (dense_rank() OVER w1 <= 2)")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: wr.sal")],
                            &[T("              ->  Seq Scan on wr")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, count(*) OVER (ORDER BY sal ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) c FROM wr) s WHERE c >= 6 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8)],
                        rows: &[
                            &[T("4"), T("7")],
                            &[T("5"), T("6")],
                            &[T("8"), T("8")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT id, count(*) OVER (ORDER BY sal ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) c FROM wr) s WHERE c >= 6;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Subquery Scan on s")],
                            &[T("  ->  WindowAgg")],
                            &[T("        Window: w1 AS (ORDER BY wr.sal ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)")],
                            &[T("        Run Condition: (count(*) OVER w1 >= 6)")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: wr.sal")],
                            &[T("              ->  Seq Scan on wr")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, count(*) OVER () c FROM wr) s WHERE c = 8 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8)],
                        rows: &[
                            &[T("1"), T("8")],
                            &[T("2"), T("8")],
                            &[T("3"), T("8")],
                            &[T("4"), T("8")],
                            &[T("5"), T("8")],
                            &[T("6"), T("8")],
                            &[T("7"), T("8")],
                            &[T("8"), T("8")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT id, count(*) OVER () c FROM wr) s WHERE 7 <= c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w1 AS ()")],
                            &[T("  Run Condition: (7 <= count(*) OVER w1)")],
                            &[T("  ->  Seq Scan on wr")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, sal, row_number() OVER (ORDER BY sal) rn, sum(sal) OVER (ORDER BY id) s FROM wr) q WHERE rn <= 3 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sal", INT4), Column("rn", INT8), Column("s", INT8)],
                        rows: &[
                            &[T("4"), T("50"), T("2"), T("550")],
                            &[T("5"), T("75"), T("3"), T("625")],
                            &[T("8"), T("10"), T("1"), T("1010")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT id, sal, row_number() OVER (ORDER BY sal) rn, sum(sal) OVER (ORDER BY id) s FROM wr) q WHERE rn <= 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w2 AS (ORDER BY wr.sal ROWS UNBOUNDED PRECEDING)")],
                            &[T("  Run Condition: (row_number() OVER w2 <= 3)")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: wr.sal")],
                            &[T("        ->  WindowAgg")],
                            &[T("              Window: w1 AS (ORDER BY wr.id)")],
                            &[T("              ->  Index Scan using wr_pkey on wr")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, ntile(4) OVER (ORDER BY id) n, cume_dist() OVER (ORDER BY id) cd FROM wr) s WHERE n < 3 AND cd < 0.5 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("n", INT4), Column("cd", FLOAT8)],
                        rows: &[
                            &[T("1"), T("1"), T("0.125")],
                            &[T("2"), T("1"), T("0.25")],
                            &[T("3"), T("2"), T("0.375")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT id, count(*) OVER (ORDER BY sal ROWS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW) c FROM wr) s WHERE c < 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Subquery Scan on s")],
                            &[T("  Filter: (s.c < 2)")],
                            &[T("  ->  WindowAgg")],
                            &[T("        Window: w1 AS (ORDER BY wr.sal ROWS BETWEEN '1'::bigint PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW)")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: wr.sal")],
                            &[T("              ->  Seq Scan on wr")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, row_number() OVER (PARTITION BY dep ORDER BY id) rn, lag(id) OVER (PARTITION BY dep ORDER BY id) l FROM wr) s WHERE rn = 1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("rn", INT8), Column("l", INT4)],
                        rows: &[
                            &[T("1"), T("1"), Null],
                            &[T("4"), T("1"), Null],
                            &[T("7"), T("1"), Null],
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
fn test_window_functions_and_aggregates_of_user_functions() {
    run_scripts(&[
        ScriptTest {
            name: "window functions and moving aggregates of user functions",
            set_up_script: &[
                "CREATE FUNCTION nth_value_def(val anyelement, n integer = 1) RETURNS anyelement LANGUAGE internal WINDOW IMMUTABLE STRICT AS 'window_nth_value';",
                "CREATE TABLE fm_w (ten int4, four int4);",
                "INSERT INTO fm_w VALUES (0, 0), (0, 0), (4, 0), (1, 1), (1, 1), (7, 3), (9, 1), (3, 3), (2, 2), (5, 1);",
                "CREATE FUNCTION logging_sfunc_nonstrict(text, anyelement) RETURNS text AS $$ SELECT COALESCE($1, '') || '*' || quote_nullable($2) $$ LANGUAGE SQL IMMUTABLE;",
                "CREATE FUNCTION logging_msfunc_nonstrict(text, anyelement) RETURNS text AS $$ SELECT COALESCE($1, '') || '+' || quote_nullable($2) $$ LANGUAGE SQL IMMUTABLE;",
                "CREATE FUNCTION logging_minvfunc_nonstrict(text, anyelement) RETURNS text AS $$ SELECT $1 || '-' || quote_nullable($2) $$ LANGUAGE SQL IMMUTABLE;",
                "CREATE AGGREGATE logging_agg_nonstrict (anyelement) (stype = text, sfunc = logging_sfunc_nonstrict, mstype = text, msfunc = logging_msfunc_nonstrict, minvfunc = logging_minvfunc_nonstrict);",
                "CREATE AGGREGATE logging_agg_nonstrict_initcond (anyelement) (stype = text, sfunc = logging_sfunc_nonstrict, mstype = text, msfunc = logging_msfunc_nonstrict, minvfunc = logging_minvfunc_nonstrict, initcond = 'I', minitcond = 'MI');",
                "CREATE FUNCTION logging_sfunc_strict(text, anyelement) RETURNS text AS $$ SELECT $1 || '*' || quote_nullable($2) $$ LANGUAGE SQL STRICT IMMUTABLE;",
                "CREATE FUNCTION logging_msfunc_strict(text, anyelement) RETURNS text AS $$ SELECT $1 || '+' || quote_nullable($2) $$ LANGUAGE SQL STRICT IMMUTABLE;",
                "CREATE FUNCTION logging_minvfunc_strict(text, anyelement) RETURNS text AS $$ SELECT $1 || '-' || quote_nullable($2) $$ LANGUAGE SQL STRICT IMMUTABLE;",
                "CREATE AGGREGATE logging_agg_strict (text) (stype = text, sfunc = logging_sfunc_strict, mstype = text, msfunc = logging_msfunc_strict, minvfunc = logging_minvfunc_strict);",
                "CREATE AGGREGATE logging_agg_strict_initcond (anyelement) (stype = text, sfunc = logging_sfunc_strict, mstype = text, msfunc = logging_msfunc_strict, minvfunc = logging_minvfunc_strict, initcond = 'I', minitcond = 'MI');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT nth_value_def(n := 2, val := ten) OVER (PARTITION BY four ORDER BY ten ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING), ten, four FROM fm_w ORDER BY four, ten;",
                    expected: Expected::Rows {
                        columns: &[Column("nth_value_def", INT4), Column("ten", INT4), Column("four", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("4"), T("0")],
                            &[T("1"), T("1"), T("1")],
                            &[T("1"), T("1"), T("1")],
                            &[T("1"), T("5"), T("1")],
                            &[T("1"), T("9"), T("1")],
                            &[Null, T("2"), T("2")],
                            &[T("7"), T("3"), T("3")],
                            &[T("7"), T("7"), T("3")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nth_value_def(ten) OVER (PARTITION BY four ORDER BY ten ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING), ten, four FROM fm_w ORDER BY four, ten;",
                    expected: Expected::Rows {
                        columns: &[Column("nth_value_def", INT4), Column("ten", INT4), Column("four", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("4"), T("0")],
                            &[T("1"), T("1"), T("1")],
                            &[T("1"), T("1"), T("1")],
                            &[T("1"), T("5"), T("1")],
                            &[T("1"), T("9"), T("1")],
                            &[T("2"), T("2"), T("2")],
                            &[T("3"), T("3"), T("3")],
                            &[T("3"), T("7"), T("3")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p::text || ',' || i::text || ':' || COALESCE(v::text, 'NULL') AS row, logging_agg_nonstrict(v) over wnd as nstrict, logging_agg_nonstrict_initcond(v) over wnd as nstrict_init, logging_agg_strict(v::text) over wnd as strict, logging_agg_strict_initcond(v) over wnd as strict_init FROM (VALUES (1, 1, NULL), (1, 2, 'a'), (1, 3, 'b'), (1, 4, NULL), (1, 5, NULL), (1, 6, 'c'), (2, 1, NULL), (2, 2, 'x'), (3, 1, 'z')) AS t(p, i, v) WINDOW wnd AS (PARTITION BY P ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY p, i;",
                    expected: Expected::Rows {
                        columns: &[Column("row", TEXT), Column("nstrict", TEXT), Column("nstrict_init", TEXT), Column("strict", TEXT), Column("strict_init", TEXT)],
                        rows: &[
                            &[T("1,1:NULL"), T("+NULL"), T("MI+NULL"), Null, T("MI")],
                            &[T("1,2:a"), T("+NULL+'a'"), T("MI+NULL+'a'"), T("a"), T("MI+'a'")],
                            &[T("1,3:b"), T("+NULL+'a'-NULL+'b'"), T("MI+NULL+'a'-NULL+'b'"), T("a+'b'"), T("MI+'a'+'b'")],
                            &[T("1,4:NULL"), T("+NULL+'a'-NULL+'b'-'a'+NULL"), T("MI+NULL+'a'-NULL+'b'-'a'+NULL"), T("a+'b'-'a'"), T("MI+'a'+'b'-'a'")],
                            &[T("1,5:NULL"), T("+NULL+'a'-NULL+'b'-'a'+NULL-'b'+NULL"), T("MI+NULL+'a'-NULL+'b'-'a'+NULL-'b'+NULL"), Null, T("MI")],
                            &[T("1,6:c"), T("+NULL+'a'-NULL+'b'-'a'+NULL-'b'+NULL-NULL+'c'"), T("MI+NULL+'a'-NULL+'b'-'a'+NULL-'b'+NULL-NULL+'c'"), T("c"), T("MI+'c'")],
                            &[T("2,1:NULL"), T("+NULL"), T("MI+NULL"), Null, T("MI")],
                            &[T("2,2:x"), T("+NULL+'x'"), T("MI+NULL+'x'"), T("x"), T("MI+'x'")],
                            &[T("3,1:z"), T("+'z'"), T("MI+'z'"), T("z"), T("MI+'z'")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p::text || ',' || i::text || ':' || CASE WHEN f THEN COALESCE(v::text, 'NULL') ELSE '-' END as row, logging_agg_nonstrict(v) filter(where f) over wnd as nstrict_filt, logging_agg_nonstrict_initcond(v) filter(where f) over wnd as nstrict_init_filt, logging_agg_strict(v::text) filter(where f) over wnd as strict_filt, logging_agg_strict_initcond(v) filter(where f) over wnd as strict_init_filt FROM (VALUES (1, 1, true, NULL), (1, 2, false, 'a'), (1, 3, true, 'b'), (1, 4, false, NULL), (1, 5, false, NULL), (1, 6, false, 'c'), (2, 1, false, NULL), (2, 2, true, 'x'), (3, 1, true, 'z')) AS t(p, i, f, v) WINDOW wnd AS (PARTITION BY p ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY p, i;",
                    expected: Expected::Rows {
                        columns: &[Column("row", TEXT), Column("nstrict_filt", TEXT), Column("nstrict_init_filt", TEXT), Column("strict_filt", TEXT), Column("strict_init_filt", TEXT)],
                        rows: &[
                            &[T("1,1:NULL"), T("+NULL"), T("MI+NULL"), Null, T("MI")],
                            &[T("1,2:-"), T("+NULL"), T("MI+NULL"), Null, T("MI")],
                            &[T("1,3:b"), T("+'b'"), T("MI+'b'"), T("b"), T("MI+'b'")],
                            &[T("1,4:-"), T("+'b'"), T("MI+'b'"), T("b"), T("MI+'b'")],
                            &[T("1,5:-"), Null, T("MI"), Null, T("MI")],
                            &[T("1,6:-"), Null, T("MI"), Null, T("MI")],
                            &[T("2,1:-"), Null, T("MI"), Null, T("MI")],
                            &[T("2,2:x"), T("+'x'"), T("MI+'x'"), T("x"), T("MI+'x'")],
                            &[T("3,1:z"), T("+'z'"), T("MI+'z'"), T("z"), T("MI+'z'")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT i::text || ':' || COALESCE(v::text, 'NULL') as row, logging_agg_strict(v::text) over wnd as inverse, logging_agg_strict(v::text || CASE WHEN random() < 0 then '?' ELSE '' END) over wnd as noinverse FROM (VALUES (1, 'a'), (2, 'b'), (3, 'c')) AS t(i, v) WINDOW wnd AS (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("row", TEXT), Column("inverse", TEXT), Column("noinverse", TEXT)],
                        rows: &[
                            &[T("1:a"), T("a"), T("a")],
                            &[T("2:b"), T("a+'b'"), T("a*'b'")],
                            &[T("3:c"), T("a+'b'-'a'+'c'"), T("b*'c'")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT i::text || ':' || COALESCE(v::text, 'NULL') as row, logging_agg_strict(v::text) filter(where true) over wnd as inverse, logging_agg_strict(v::text) filter(where random() >= 0) over wnd as noinverse FROM (VALUES (1, 'a'), (2, 'b'), (3, 'c')) AS t(i, v) WINDOW wnd AS (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("row", TEXT), Column("inverse", TEXT), Column("noinverse", TEXT)],
                        rows: &[
                            &[T("1:a"), T("a"), T("a")],
                            &[T("2:b"), T("a+'b'"), T("a*'b'")],
                            &[T("3:c"), T("a+'b'-'a'+'c'"), T("b*'c'")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT logging_agg_strict(v::text) OVER wnd FROM (VALUES (1, 'a'), (2, 'b'), (3, 'c')) AS t(i, v) WINDOW wnd AS (ORDER BY i ROWS BETWEEN CURRENT ROW AND CURRENT ROW) ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("logging_agg_strict", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
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
