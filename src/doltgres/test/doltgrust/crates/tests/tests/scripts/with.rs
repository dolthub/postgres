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
fn test_with_statements() {
    run_scripts(&[
        ScriptTest {
            name: "basic values statements",
            set_up_script: &[
                "create table t (i int primary key);",
                "insert into t values (1), (2), (3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "with cte as (select 1) select * from cte;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "with cte as (select 1, 2, 3 union select 4, 5, 6) select * from cte;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4), Column("?column?", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("4"), T("5"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "with cte as (values (1)) select * from cte;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "with cte as (values (1, 2, 3) union values (4, 5, 6)) select * from cte;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4), Column("column2", INT4), Column("column3", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("4"), T("5"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "with cte as (select 1, 2, 3 union values (4, 5, 6)) select * from cte;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4), Column("?column?", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("4"), T("5"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "with recursive cte(x) as (select 1 union all select x + 1 from cte) select * from cte limit 5;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
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
fn test_recursive_query_form_rules() {
    run_scripts(&[
        ScriptTest {
            name: "malformed recursive queries",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"WITH x(n, b) AS (SELECT 1)
SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"WITH query "x" has 1 columns available but 2 columns specified"#, position: 6, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 INTERSECT SELECT n+1 FROM x)
	SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive query "x" does not have the form non-recursive-term UNION [ALL] recursive-term"#, position: 16, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 INTERSECT ALL SELECT n+1 FROM x)
	SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive query "x" does not have the form non-recursive-term UNION [ALL] recursive-term"#, position: 16, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 EXCEPT SELECT n+1 FROM x)
	SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive query "x" does not have the form non-recursive-term UNION [ALL] recursive-term"#, position: 16, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 EXCEPT ALL SELECT n+1 FROM x)
	SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive query "x" does not have the form non-recursive-term UNION [ALL] recursive-term"#, position: 16, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT n FROM x)
	SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive query "x" does not have the form non-recursive-term UNION [ALL] recursive-term"#, position: 16, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT n FROM x UNION ALL SELECT 1)
	SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "x" must not appear within its non-recursive term"#, position: 39, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE y (a INTEGER);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO y SELECT generate_series(1, 10);",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT a FROM y WHERE a = 1
	UNION ALL
	SELECT x.n+1 FROM y LEFT JOIN x ON x.n = y.a WHERE n < 10)
SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "x" must not appear within an outer join"#, position: 95, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT a FROM y WHERE a = 1
	UNION ALL
	SELECT x.n+1 FROM x RIGHT JOIN y ON x.n = y.a WHERE n < 10)
SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "x" must not appear within an outer join"#, position: 83, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT a FROM y WHERE a = 1
	UNION ALL
	SELECT x.n+1 FROM x FULL JOIN y ON x.n = y.a WHERE n < 10)
SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "x" must not appear within an outer join"#, position: 83, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM x
                          WHERE n IN (SELECT * FROM x))
  SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "x" must not appear within a subquery"#, position: 114, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT count(*) FROM x)
  SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: "aggregate functions are not allowed in a recursive query's recursive term", position: 51, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT sum(n) FROM x)
  SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: "aggregate functions are not allowed in a recursive query's recursive term", position: 51, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM x ORDER BY 1)
  SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "ORDER BY in a recursive query is not implemented", position: 71, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM x LIMIT 10 OFFSET 1)
  SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "OFFSET in a recursive query is not implemented", position: 78, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM x FOR UPDATE)
  SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR UPDATE/SHARE in a recursive query is not implemented", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE x(id) AS (values (1)
    UNION ALL
    SELECT (SELECT * FROM x) FROM x WHERE id < 5
) SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "x" must not appear within a subquery"#, position: 77, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE
  x (id) AS (SELECT 1 UNION ALL SELECT id+1 FROM y WHERE id < 5),
  y (id) AS (SELECT 1 UNION ALL SELECT id+1 FROM x WHERE id < 5)
SELECT * FROM x;"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "mutual recursion between WITH items is not implemented", position: 18, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE foo(i) AS
    (values (1)
    UNION ALL
       (SELECT i+1 FROM foo WHERE i < 10
          UNION ALL
       SELECT i+1 FROM foo WHERE i < 5)
) SELECT * FROM foo;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "foo" must not appear more than once"#, position: 140, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE foo(i) AS
    (values (1)
    UNION ALL
	   SELECT * FROM
       (SELECT i+1 FROM foo WHERE i < 10
          UNION ALL
       SELECT i+1 FROM foo WHERE i < 5) AS t
) SELECT * FROM foo;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "foo" must not appear more than once"#, position: 158, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE foo(i) AS
    (values (1)
    UNION ALL
       (SELECT i+1 FROM foo WHERE i < 10
          EXCEPT
       SELECT i+1 FROM foo WHERE i < 5)
) SELECT * FROM foo;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "foo" must not appear within EXCEPT"#, position: 137, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE foo(i) AS
    (values (1)
    UNION ALL
       (SELECT i+1 FROM foo WHERE i < 10
          INTERSECT
       SELECT i+1 FROM foo WHERE i < 5)
) SELECT * FROM foo;"#,
                    expected: Expected::Error(Diagnostic { code: "42P19", message: r#"recursive reference to query "foo" must not appear more than once"#, position: 140, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE foo(i) AS
   (SELECT i FROM (VALUES(1),(2)) t(i)
   UNION ALL
   SELECT (i+1)::numeric(10,0) FROM foo WHERE i < 10)
SELECT * FROM foo;"#,
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"recursive query "foo" column 1 has type integer in non-recursive term but type numeric overall"#, hint: "Cast the output of the non-recursive term to the correct type.", position: 37, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"WITH RECURSIVE foo(i) AS
   (SELECT i::numeric(3,0) FROM (VALUES(1),(2)) t(i)
   UNION ALL
   SELECT (i+1)::numeric(10,0) FROM foo WHERE i < 10)
SELECT * FROM foo;"#,
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"recursive query "foo" column 1 has type numeric(3,0) in non-recursive term but type numeric overall"#, hint: "Cast the output of the non-recursive term to the correct type.", position: 37, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_recursive_union_of_unhashable_types() {
    run_scripts(&[
        ScriptTest {
            name: "a recursive UNION over a type without a hash function",
            assertions: &[
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (VALUES ('01'::varbit) UNION SELECT n || '10'::varbit FROM t WHERE n < '100'::varbit) SELECT n FROM t;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "could not implement recursive UNION", detail: "All column datatypes must be hashable.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_with_queries_planned() {
    run_scripts(&[
        ScriptTest {
            name: "WITH queries inlined as subqueries or planned once as initplans, with CTE scans",
            set_up_script: &[
                "CREATE TABLE wt (a int PRIMARY KEY, b text) WITH (autovacuum_enabled = false);",
                "INSERT INTO wt VALUES (1, 'x'), (2, 'y'), (3, 'z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "WITH c AS (SELECT a, b FROM wt WHERE b > 'x') SELECT * FROM c ORDER BY a;",
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
                    query: "EXPLAIN (COSTS OFF) WITH c AS (SELECT a, b FROM wt WHERE b > 'x') SELECT * FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on wt")],
                            &[T("  Filter: (b > 'x'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c AS MATERIALIZED (SELECT a, b FROM wt) SELECT * FROM c WHERE a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) WITH c AS MATERIALIZED (SELECT a, b FROM wt) SELECT * FROM c WHERE a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("CTE Scan on c")],
                            &[T("  Filter: (a = 2)")],
                            &[T("  CTE c")],
                            &[T("    ->  Seq Scan on wt")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c AS (SELECT a FROM wt) SELECT x.a, y.a FROM c x JOIN c y ON x.a = y.a + 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) WITH c AS (SELECT a FROM wt) SELECT x.a, y.a FROM c x JOIN c y ON x.a = y.a + 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Merge Join")],
                            &[T("  Merge Cond: (x.a = ((y.a + 1)))")],
                            &[T("  CTE c")],
                            &[T("    ->  Seq Scan on wt")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: x.a")],
                            &[T("        ->  CTE Scan on c x")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: ((y.a + 1))")],
                            &[T("        ->  CTE Scan on c y")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c AS NOT MATERIALIZED (SELECT a FROM wt) SELECT x.a, y.a FROM c x JOIN c y ON x.a = y.a ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) WITH c AS NOT MATERIALIZED (SELECT a FROM wt) SELECT x.a, y.a FROM c x JOIN c y ON x.a = y.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on wt")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c AS (SELECT a, random() AS r FROM wt) SELECT count(DISTINCT r) FROM (SELECT r FROM c UNION ALL SELECT r FROM c) s;",
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
                    query: "WITH c1 AS (SELECT a FROM wt), c2 AS (SELECT a * 10 AS a FROM c1) SELECT * FROM c2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                            &[T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c1 AS MATERIALIZED (SELECT a FROM wt), c2 AS MATERIALIZED (SELECT a * 10 AS a FROM c1) SELECT * FROM c2, c1 WHERE c2.a = c1.a * 10 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("20"), T("2")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) WITH c1 AS MATERIALIZED (SELECT a FROM wt), c2 AS MATERIALIZED (SELECT a * 10 AS a FROM c1) SELECT * FROM c2, c1 WHERE c2.a = c1.a * 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Merge Join")],
                            &[T("  Merge Cond: (c2.a = ((c1.a * 10)))")],
                            &[T("  CTE c1")],
                            &[T("    ->  Seq Scan on wt")],
                            &[T("  CTE c2")],
                            &[T("    ->  CTE Scan on c1 c1_1")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: c2.a")],
                            &[T("        ->  CTE Scan on c2")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: ((c1.a * 10))")],
                            &[T("        ->  CTE Scan on c1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, (WITH c AS MATERIALIZED (SELECT wt.a + 1 AS x) SELECT x FROM c) FROM wt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("x", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wt WHERE a IN (WITH c AS MATERIALIZED (SELECT 2 AS v UNION ALL SELECT 3) SELECT v FROM c) ORDER BY a;",
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
                    query: "WITH unused AS (SELECT 1/0) SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c AS (SELECT 1 AS x) SELECT * FROM c, LATERAL (SELECT x + 1) l;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c (p, q) AS (SELECT a, b FROM wt) SELECT q, p FROM c ORDER BY p DESC LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("q", TEXT), Column("p", INT4)],
                        rows: &[
                            &[T("z"), T("3")],
                            &[T("y"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c AS (SELECT a FROM wt) SELECT (SELECT max(a) FROM c), (SELECT min(a) FROM c);",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT4), Column("min", INT4)],
                        rows: &[
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c AS (SELECT a FROM wt FOR UPDATE) SELECT count(*) FROM c;",
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
                    query: "EXPLAIN (VERBOSE, COSTS OFF) WITH c AS MATERIALIZED (SELECT a, b FROM wt) SELECT b FROM c WHERE a > 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("CTE Scan on c")],
                            &[T("  Output: c.b")],
                            &[T("  Filter: (c.a > 1)")],
                            &[T("  CTE c")],
                            &[T("    ->  Seq Scan on public.wt")],
                            &[T("          Output: wt.a, wt.b")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (FORMAT JSON, COSTS OFF) WITH c AS MATERIALIZED (SELECT a FROM wt) SELECT * FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", JSON)],
                        rows: &[
                            &[T("[\n  {\n    \"Plan\": {\n      \"Node Type\": \"CTE Scan\",\n      \"Parallel Aware\": false,\n      \"Async Capable\": false,\n      \"CTE Name\": \"c\",\n      \"Alias\": \"c\",\n      \"Disabled\": false,\n      \"Plans\": [\n        {\n          \"Node Type\": \"Seq Scan\",\n          \"Parent Relationship\": \"InitPlan\",\n          \"Subplan Name\": \"CTE c\",\n          \"Parallel Aware\": false,\n          \"Async Capable\": false,\n          \"Relation Name\": \"wt\",\n          \"Alias\": \"wt\",\n          \"Disabled\": false\n        }\n      ]\n    }\n  }\n]")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH c AS MATERIALIZED (SELECT a FROM wt ORDER BY a DESC) SELECT * FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE cur SCROLL CURSOR FOR WITH c AS MATERIALIZED (SELECT a FROM wt ORDER BY a) SELECT * FROM c;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM cur;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 FROM cur;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM cur;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM cur;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) WITH c AS MATERIALIZED (SELECT a FROM wt) SELECT * FROM c x, c y WHERE x.a = y.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Merge Join (actual rows=3.00 loops=1)")],
                            &[T("  Merge Cond: (x.a = y.a)")],
                            &[T("  CTE c")],
                            &[T("    ->  Seq Scan on wt (actual rows=3.00 loops=1)")],
                            &[T("  ->  Sort (actual rows=3.00 loops=1)")],
                            &[T("        Sort Key: x.a")],
                            &[T("        Sort Method: quicksort  Memory: 25kB")],
                            &[T("        ->  CTE Scan on c x (actual rows=3.00 loops=1)")],
                            &[T("              Storage: Memory  Maximum Storage: 17kB")],
                            &[T("  ->  Sort (actual rows=3.00 loops=1)")],
                            &[T("        Sort Key: y.a")],
                            &[T("        Sort Method: quicksort  Memory: 25kB")],
                            &[T("        ->  CTE Scan on c y (actual rows=3.00 loops=1)")],
                            &[T("              Storage: Memory  Maximum Storage: 17kB")],
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
fn test_with_recursive_planned() {
    run_scripts(&[
        ScriptTest {
            name: "WITH RECURSIVE by RecursiveUnion and WorkTableScan, with SEARCH and CYCLE",
            set_up_script: &[
                "CREATE TABLE tree (id int PRIMARY KEY, parent int) WITH (autovacuum_enabled = false);",
                "INSERT INTO tree VALUES (1, NULL), (2, 1), (3, 1), (4, 2), (5, 4), (6, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 5) SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (VALUES (1) UNION ALL SELECT n + 1 FROM t WHERE n < 100) SELECT sum(n) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("5050")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 UNION SELECT (n % 3) + 1 FROM t) SELECT * FROM t ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
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
                    query: "WITH RECURSIVE sub(id, depth) AS (SELECT id, 0 FROM tree WHERE id = 1 UNION ALL SELECT tree.id, depth + 1 FROM tree JOIN sub ON tree.parent = sub.id) SELECT * FROM sub ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("depth", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                            &[T("4"), T("2")],
                            &[T("5"), T("3")],
                            &[T("6"), T("2")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) WITH RECURSIVE sub(id, depth) AS (SELECT id, 0 FROM tree WHERE id = 1 UNION ALL SELECT tree.id, depth + 1 FROM tree JOIN sub ON tree.parent = sub.id) SELECT * FROM sub;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("CTE Scan on sub")],
                            &[T("  CTE sub")],
                            &[T("    ->  Recursive Union")],
                            &[T("          ->  Index Only Scan using tree_pkey on tree")],
                            &[T("                Index Cond: (id = 1)")],
                            &[T("          ->  Hash Join")],
                            &[T("                Hash Cond: (tree_1.parent = sub_1.id)")],
                            &[T("                ->  Seq Scan on tree tree_1")],
                            &[T("                ->  Hash")],
                            &[T("                      ->  WorkTable Scan on sub sub_1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) WITH RECURSIVE t(n) AS (SELECT 1 UNION SELECT (n % 3) + 1 FROM t) SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("CTE Scan on t")],
                            &[T("  CTE t")],
                            &[T("    ->  Recursive Union")],
                            &[T("          ->  Result")],
                            &[T("          ->  WorkTable Scan on t t_1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t) SELECT * FROM t LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
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
                    query: "WITH RECURSIVE t(n, path) AS (SELECT id, ARRAY[id] FROM tree WHERE parent IS NULL UNION ALL SELECT tree.id, path || tree.id FROM tree, t WHERE tree.parent = t.n) SELECT * FROM t ORDER BY path;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("path", INT4_ARRAY)],
                        rows: &[
                            &[T("1"), T("{1}")],
                            &[T("2"), T("{1,2}")],
                            &[T("4"), T("{1,2,4}")],
                            &[T("5"), T("{1,2,4,5}")],
                            &[T("3"), T("{1,3}")],
                            &[T("6"), T("{1,3,6}")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 3) SELECT a.n, b.n FROM t a, t b ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                            &[T("1"), T("3")],
                            &[T("2"), T("1")],
                            &[T("2"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), T("1")],
                            &[T("3"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, (WITH RECURSIVE up(i) AS (SELECT tree.parent UNION ALL SELECT tree2.parent FROM tree tree2, up WHERE tree2.id = up.i) SELECT count(*) FROM up WHERE i IS NOT NULL) FROM tree ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                            &[T("4"), T("2")],
                            &[T("5"), T("3")],
                            &[T("6"), T("2")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE search_tree(id, parent) AS (SELECT id, parent FROM tree WHERE id = 1 UNION ALL SELECT t.id, t.parent FROM tree t, search_tree st WHERE t.parent = st.id) SEARCH DEPTH FIRST BY id SET ordercol SELECT * FROM search_tree ORDER BY ordercol;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("parent", INT4), Column("ordercol", RECORD_ARRAY)],
                        rows: &[
                            &[T("1"), Null, T("{(1)}")],
                            &[T("2"), T("1"), T("{(1),(2)}")],
                            &[T("4"), T("2"), T("{(1),(2),(4)}")],
                            &[T("5"), T("4"), T("{(1),(2),(4),(5)}")],
                            &[T("3"), T("1"), T("{(1),(3)}")],
                            &[T("6"), T("3"), T("{(1),(3),(6)}")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE search_tree(id, parent) AS (SELECT id, parent FROM tree WHERE id = 1 UNION ALL SELECT t.id, t.parent FROM tree t, search_tree st WHERE t.parent = st.id) SEARCH BREADTH FIRST BY id SET ordercol SELECT * FROM search_tree ORDER BY ordercol;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("parent", INT4), Column("ordercol", RECORD)],
                        rows: &[
                            &[T("1"), Null, T("(0,1)")],
                            &[T("2"), T("1"), T("(1,2)")],
                            &[T("3"), T("1"), T("(1,3)")],
                            &[T("4"), T("2"), T("(2,4)")],
                            &[T("6"), T("3"), T("(2,6)")],
                            &[T("5"), T("4"), T("(3,5)")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 5) SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("CTE Scan on t")],
                            &[T("  Output: t.n")],
                            &[T("  CTE t")],
                            &[T("    ->  Recursive Union")],
                            &[T("          ->  Result")],
                            &[T("                Output: 1")],
                            &[T("          ->  WorkTable Scan on t t_1")],
                            &[T("                Output: (t_1.n + 1)")],
                            &[T("                Filter: (t_1.n < 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 3), u AS (SELECT n * 10 AS m FROM t) SELECT * FROM u, t WHERE m = n * 10 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("m", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("20"), T("2")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 3) SELECT n FROM t UNION SELECT 10 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("10")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE g(id, path) AS (SELECT 1, ARRAY[ROW(1, 0)] UNION SELECT tree.id, path || ROW(tree.id, g.id) FROM tree, g WHERE tree.parent = g.id) SELECT id, path FROM g ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("path", RECORD_ARRAY)],
                        rows: &[
                            &[T("1"), T("{\"(1,0)\"}")],
                            &[T("2"), T("{\"(1,0)\",\"(2,1)\"}")],
                            &[T("3"), T("{\"(1,0)\",\"(3,1)\"}")],
                            &[T("4"), T("{\"(1,0)\",\"(2,1)\",\"(4,2)\"}")],
                            &[T("5"), T("{\"(1,0)\",\"(2,1)\",\"(4,2)\",\"(5,4)\"}")],
                            &[T("6"), T("{\"(1,0)\",\"(3,1)\",\"(6,3)\"}")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hash_array(ARRAY[ROW(1, 2)]), hash_record(ROW(1, 2));",
                    expected: Expected::Rows {
                        columns: &[Column("hash_array", INT4), Column("hash_record", INT4)],
                        rows: &[
                            &[T("-2087801201"), T("-2087801232")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hash_record(ROW(ROW(1, 2)));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "could not identify a hash function for type record", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t) SELECT count(*) FROM (SELECT * FROM t LIMIT 1000) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT 1 FROM t, t t2) SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "42P19", message: "recursive reference to query \"t\" must not appear more than once", position: 61, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE t(n) AS (SELECT 1 INTERSECT SELECT n + 1 FROM t) SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "42P19", message: "recursive query \"t\" does not have the form non-recursive-term UNION [ALL] recursive-term", position: 16, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) WITH RECURSIVE test AS (SELECT 1 AS x UNION ALL SELECT x + 1 FROM test) SEARCH BREADTH FIRST BY x SET y SELECT * FROM test LIMIT 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  Output: test.x, test.y")],
                            &[T("  CTE test")],
                            &[T("    ->  Recursive Union")],
                            &[T("          ->  Result")],
                            &[T("                Output: 1, '(0,1)'::record")],
                            &[T("          ->  WorkTable Scan on test test_1")],
                            &[T("                Output: (test_1.x + 1), ROW(int8inc((test_1.y).\"*DEPTH*\"), (test_1.x + 1))")],
                            &[T("  ->  CTE Scan on test")],
                            &[T("        Output: test.x, test.y")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE test AS (SELECT 1 AS x UNION ALL SELECT x + 1 FROM test) SEARCH BREADTH FIRST BY x SET y SELECT * FROM test LIMIT 5;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", RECORD)],
                        rows: &[
                            &[T("1"), T("(0,1)")],
                            &[T("2"), T("(1,2)")],
                            &[T("3"), T("(2,3)")],
                            &[T("4"), T("(3,4)")],
                            &[T("5"), T("(4,5)")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE tmp_w (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM tmp_w;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on pg_temp.tmp_w")],
                            &[T("  Output: a")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tree VALUES (7, 6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tree SET parent = 7 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE g(id) AS (SELECT 1 UNION ALL SELECT tree.id FROM tree, g WHERE tree.parent = g.id) CYCLE id SET is_cycle USING path SELECT * FROM g;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("is_cycle", BOOL), Column("path", RECORD_ARRAY)],
                        rows: &[
                            &[T("1"), T("f"), T("{(1)}")],
                            &[T("2"), T("f"), T("{(1),(2)}")],
                            &[T("3"), T("f"), T("{(1),(3)}")],
                            &[T("4"), T("f"), T("{(1),(2),(4)}")],
                            &[T("6"), T("f"), T("{(1),(3),(6)}")],
                            &[T("5"), T("f"), T("{(1),(2),(4),(5)}")],
                            &[T("7"), T("f"), T("{(1),(3),(6),(7)}")],
                            &[T("1"), T("t"), T("{(1),(3),(6),(7),(1)}")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_with_data_modifying() {
    run_scripts(&[
        ScriptTest {
            name: "INSERT, UPDATE, and DELETE in WITH share the statement's snapshot and run to their end",
            set_up_script: &[
                "CREATE TABLE mt (a int PRIMARY KEY, b text) WITH (autovacuum_enabled = false);",
                "CREATE TABLE mlog (a int, b text) WITH (autovacuum_enabled = false);",
                "INSERT INTO mt VALUES (1, 'x'), (2, 'y'), (3, 'z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "WITH ins AS (INSERT INTO mt VALUES (4, 'w') RETURNING *) SELECT * FROM ins;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("4"), T("w")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH ins AS (INSERT INTO mt VALUES (5, 'v') RETURNING a) SELECT (SELECT count(*) FROM mt) AS before, (SELECT count(*) FROM ins) AS inserted;",
                    expected: Expected::Rows {
                        columns: &[Column("before", INT8), Column("inserted", INT8)],
                        rows: &[
                            &[T("4"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH upd AS (UPDATE mt SET b = b || '!' WHERE a <= 2 RETURNING a, b) SELECT a, b FROM upd ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x!")],
                            &[T("2"), T("y!")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH upd AS (UPDATE mt SET b = 'seen' WHERE a = 3 RETURNING a) SELECT b FROM mt WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("z")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM mt WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("seen")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH del AS (DELETE FROM mt WHERE a = 5 RETURNING *) INSERT INTO mlog SELECT * FROM del;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mlog ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("5"), T("v")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH quiet AS (INSERT INTO mlog VALUES (100, 'never read')) SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mlog WHERE a = 100;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH lim AS (INSERT INTO mlog SELECT g, 'g' FROM generate_series(200, 204) g RETURNING a) SELECT a FROM lim LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mlog WHERE a >= 200;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH u1 AS (UPDATE mt SET b = 'first' WHERE a = 1 RETURNING a), u2 AS (UPDATE mt SET b = 'second' WHERE a = 1 RETURNING a) SELECT (SELECT count(*) FROM u1), (SELECT count(*) FROM u2);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM mt WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("first")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH d AS (DELETE FROM mt WHERE a = 4 RETURNING a), i AS (INSERT INTO mt VALUES (4, 'again') RETURNING a) SELECT (SELECT count(*) FROM d), (SELECT count(*) FROM i);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("first")],
                            &[T("2"), T("y!")],
                            &[T("3"), T("seen")],
                            &[T("4"), T("again")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) WITH ins AS (INSERT INTO mlog VALUES (300, 'e') RETURNING *) SELECT * FROM ins;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("CTE Scan on ins")],
                            &[T("  CTE ins")],
                            &[T("    ->  Insert on mlog")],
                            &[T("          ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (WITH ins AS (INSERT INTO mlog VALUES (400, 'nested') RETURNING *) SELECT * FROM ins) s;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "WITH clause containing a data-modifying statement must be at the top level", position: 21, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH t AS (SELECT 1 AS x) INSERT INTO mlog SELECT x, 'from cte' FROM t RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("from cte")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH ins AS (INSERT INTO mlog VALUES (500, 'twice') RETURNING a) SELECT x.a, y.a FROM ins x, ins y;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("500"), T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 3), ins AS (INSERT INTO mlog SELECT n, 'rec' FROM r RETURNING a) SELECT count(*) FROM ins;",
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
                    query: "WITH upd AS (UPDATE mt SET b = 'u' WHERE a = 2 RETURNING *) DELETE FROM mt WHERE a IN (SELECT a FROM upd) RETURNING a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[],
                        tag: "DELETE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("first")],
                            &[T("2"), T("u")],
                            &[T("3"), T("seen")],
                            &[T("4"), T("again")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ON CONFLICT DO UPDATE skips a row of a table without a primary key that its WITH query updated",
            set_up_script: &[
                "CREATE TABLE withz (k int UNIQUE, v text);",
                "INSERT INTO withz VALUES (2, 'x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "WITH simpletup AS (SELECT 2 k, 'Green' v), upsert_cte AS (INSERT INTO withz VALUES (2, 'Blue') ON CONFLICT (k) DO UPDATE SET (k, v) = (SELECT k, v FROM simpletup WHERE simpletup.k = withz.k) RETURNING k, v) INSERT INTO withz VALUES (2, 'Red') ON CONFLICT (k) DO UPDATE SET (k, v) = (SELECT k, v FROM upsert_cte WHERE upsert_cte.k = withz.k) RETURNING k, v;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "INSERT 0 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM withz;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("Green")],
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
