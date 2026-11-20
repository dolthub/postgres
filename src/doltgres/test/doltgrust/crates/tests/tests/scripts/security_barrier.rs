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
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_security_barrier_views() {
    run_scripts(&[
        ScriptTest {
            name: "UPDATE, DELETE, and SELECT through security_barrier views, with leaky and leakproof conditions",
            set_up_script: &[
                "CREATE FUNCTION snoop(anyelement) RETURNS boolean LANGUAGE plpgsql COST 0.000001 AS $$ BEGIN RAISE NOTICE 'snooped value: %', $1; RETURN true; END $$;",
                "CREATE FUNCTION leakproof(anyelement) RETURNS boolean LANGUAGE plpgsql STRICT IMMUTABLE LEAKPROOF AS 'begin return true; end';",
                "CREATE TABLE base_tbl (id int PRIMARY KEY, person text, visibility text);",
                "INSERT INTO base_tbl VALUES (1, 'Tom', 'public'), (2, 'Dick', 'private'), (3, 'Harry', 'public');",
                "CREATE TABLE other_tbl (id int, note text);",
                "INSERT INTO other_tbl VALUES (1, 'x'), (3, 'y');",
                "CREATE VIEW plain_view AS SELECT id, person FROM base_tbl WHERE visibility = 'public';",
                "CREATE VIEW rw_view1 WITH (security_barrier = true) AS SELECT id, person FROM base_tbl WHERE visibility = 'public';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM rw_view1 WHERE snoop(person) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("person", TEXT)],
                        rows: &[
                            &[T("1"), T("Tom")],
                            &[T("3"), T("Harry")],
                        ],
                        tag: "SELECT 2",
                    },
                    notices: &[Diagnostic { code: "00000", message: "snooped value: Tom", ..N }, Diagnostic { code: "00000", message: "snooped value: Harry", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM plain_view WHERE snoop(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on base_tbl"#)],
                            &[T(r#"  Filter: (snoop(person) AND (visibility = 'public'::text))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rw_view1 WHERE snoop(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Subquery Scan on rw_view1"#)],
                            &[T(r#"  Filter: snoop(rw_view1.person)"#)],
                            &[T(r#"  ->  Seq Scan on base_tbl"#)],
                            &[T(r#"        Filter: (visibility = 'public'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rw_view1 WHERE leakproof(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on base_tbl"#)],
                            &[T(r#"  Filter: ((visibility = 'public'::text) AND leakproof(person))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rw_view1 WHERE snoop(person) AND leakproof(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Subquery Scan on rw_view1"#)],
                            &[T(r#"  Filter: snoop(rw_view1.person)"#)],
                            &[T(r#"  ->  Seq Scan on base_tbl"#)],
                            &[T(r#"        Filter: ((visibility = 'public'::text) AND leakproof(person))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rw_view1 WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using base_tbl_pkey on base_tbl"#)],
                            &[T(r#"  Index Cond: (id = 1)"#)],
                            &[T(r#"  Filter: (visibility = 'public'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rw_view1 WHERE id = 1 AND snoop(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Subquery Scan on rw_view1"#)],
                            &[T(r#"  Filter: snoop(rw_view1.person)"#)],
                            &[T(r#"  ->  Index Scan using base_tbl_pkey on base_tbl"#)],
                            &[T(r#"        Index Cond: (id = 1)"#)],
                            &[T(r#"        Filter: (visibility = 'public'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rw_view1 v JOIN other_tbl o ON v.id = o.id WHERE snoop(v.person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Hash Join"#)],
                            &[T(r#"  Hash Cond: (o.id = v.id)"#)],
                            &[T(r#"  ->  Seq Scan on other_tbl o"#)],
                            &[T(r#"  ->  Hash"#)],
                            &[T(r#"        ->  Subquery Scan on v"#)],
                            &[T(r#"              Filter: snoop(v.person)"#)],
                            &[T(r#"              ->  Seq Scan on base_tbl"#)],
                            &[T(r#"                    Filter: (visibility = 'public'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rw_view1 v JOIN other_tbl o ON v.id = o.id AND v.person = o.note;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Hash Join"#)],
                            &[T(r#"  Hash Cond: ((o.id = base_tbl.id) AND (o.note = base_tbl.person))"#)],
                            &[T(r#"  ->  Seq Scan on other_tbl o"#)],
                            &[T(r#"  ->  Hash"#)],
                            &[T(r#"        ->  Seq Scan on base_tbl"#)],
                            &[T(r#"              Filter: (visibility = 'public'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rw_view1 v JOIN other_tbl o ON v.id = o.id ORDER BY v.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("person", TEXT), Column("id", INT4), Column("note", TEXT)],
                        rows: &[
                            &[T("1"), T("Tom"), T("1"), T("x")],
                            &[T("3"), T("Harry"), T("3"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW rw_view2 WITH (security_barrier = true) AS SELECT * FROM rw_view1 WHERE snoop(person);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM rw_view2 WHERE snoop(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Subquery Scan on rw_view2"#)],
                            &[T(r#"  Filter: snoop(rw_view2.person)"#)],
                            &[T(r#"  ->  Subquery Scan on rw_view1"#)],
                            &[T(r#"        Filter: snoop(rw_view1.person)"#)],
                            &[T(r#"        ->  Seq Scan on base_tbl"#)],
                            &[T(r#"              Filter: (visibility = 'public'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rw_view2 WHERE id = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("person", TEXT)],
                        rows: &[
                            &[T("3"), T("Harry")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "snooped value: Harry", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE rw_view1 SET person = person WHERE snoop(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on base_tbl"#)],
                            &[T(r#"  ->  Seq Scan on base_tbl"#)],
                            &[T(r#"        Filter: ((visibility = 'public'::text) AND snoop(person))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM rw_view1 WHERE snoop(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Delete on base_tbl"#)],
                            &[T(r#"  ->  Seq Scan on base_tbl"#)],
                            &[T(r#"        Filter: ((visibility = 'public'::text) AND snoop(person))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE rw_view2 SET person = person WHERE snoop(person);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on base_tbl"#)],
                            &[T(r#"  ->  Seq Scan on base_tbl"#)],
                            &[T(r#"        Filter: ((visibility = 'public'::text) AND snoop(person) AND snoop(person))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rw_view1 SET person = upper(person) WHERE id = 1 AND snoop(person);",
                    expected: Expected::Tag("UPDATE 1"),
                    notices: &[Diagnostic { code: "00000", message: "snooped value: Tom", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rw_view1 SET person = 'nobody' WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rw_view1 WHERE id = 3 AND snoop(person);",
                    expected: Expected::Tag("DELETE 1"),
                    notices: &[Diagnostic { code: "00000", message: "snooped value: Harry", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM base_tbl ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("person", TEXT), Column("visibility", TEXT)],
                        rows: &[
                            &[T("1"), T("TOM"), T("public")],
                            &[T("2"), T("Dick"), T("private")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (a int, b float, c text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t1_a_idx ON t1 (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 SELECT i, i, 't1' FROM generate_series(1, 10) g(i);",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t11 (d text) INHERITS (t1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t11_a_idx ON t11 (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t11 SELECT i, i, 't11', 't11d' FROM generate_series(1, 10) g(i);",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE t1;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE t11;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v1 WITH (security_barrier = true) AS SELECT *, (SELECT d FROM t11 WHERE t11.a = t1.a LIMIT 1) AS d FROM t1 WHERE a > 5 AND EXISTS (SELECT 1 FROM t11 WHERE t11.a = t1.a);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", FLOAT8), Column("c", TEXT), Column("d", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 WHERE a = 8;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", FLOAT8), Column("c", TEXT), Column("d", TEXT)],
                        rows: &[
                            &[T("8"), T("8"), T("t1"), T("t11d")],
                            &[T("8"), T("8"), T("t11"), T("t11d")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) UPDATE v1 SET a = 100 WHERE snoop(a) AND leakproof(a) AND a < 7 AND a != 6;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on public.t1"#)],
                            &[T(r#"  Update on public.t1 t1_1"#)],
                            &[T(r#"  Update on public.t11 t1_2"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        Output: 100, t1.tableoid, t1.ctid"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Index Scan using t1_a_idx on public.t1 t1_1"#)],
                            &[T(r#"                    Output: t1_1.tableoid, t1_1.ctid"#)],
                            &[T(r#"                    Index Cond: ((t1_1.a > 5) AND (t1_1.a < 7))"#)],
                            &[T(r#"                    Filter: ((t1_1.a <> 6) AND (ANY (t1_1.a = (hashed SubPlan 2).col1)) AND snoop(t1_1.a) AND leakproof(t1_1.a))"#)],
                            &[T(r#"                    SubPlan 2"#)],
                            &[T(r#"                      ->  Seq Scan on public.t11"#)],
                            &[T(r#"                            Output: t11.a"#)],
                            &[T(r#"              ->  Index Scan using t11_a_idx on public.t11 t1_2"#)],
                            &[T(r#"                    Output: t1_2.tableoid, t1_2.ctid"#)],
                            &[T(r#"                    Index Cond: ((t1_2.a > 5) AND (t1_2.a < 7))"#)],
                            &[T(r#"                    Filter: ((t1_2.a <> 6) AND (ANY (t1_2.a = (hashed SubPlan 2).col1)) AND snoop(t1_2.a) AND leakproof(t1_2.a))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE v1 SET a = 100 WHERE snoop(a) AND leakproof(a) AND a < 7 AND a != 6;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1 WHERE a = 100 ORDER BY c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", FLOAT8), Column("c", TEXT), Column("d", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 WHERE a = 100 ORDER BY c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", FLOAT8), Column("c", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) UPDATE v1 SET a = a + 1 WHERE snoop(a) AND leakproof(a) AND a = 8;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on public.t1"#)],
                            &[T(r#"  Update on public.t1 t1_1"#)],
                            &[T(r#"  Update on public.t11 t1_2"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        Output: (t1.a + 1), t1.tableoid, t1.ctid"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Index Scan using t1_a_idx on public.t1 t1_1"#)],
                            &[T(r#"                    Output: t1_1.a, t1_1.tableoid, t1_1.ctid"#)],
                            &[T(r#"                    Index Cond: ((t1_1.a > 5) AND (t1_1.a = 8))"#)],
                            &[T(r#"                    Filter: ((ANY (t1_1.a = (hashed SubPlan 2).col1)) AND snoop(t1_1.a) AND leakproof(t1_1.a))"#)],
                            &[T(r#"                    SubPlan 2"#)],
                            &[T(r#"                      ->  Seq Scan on public.t11"#)],
                            &[T(r#"                            Output: t11.a"#)],
                            &[T(r#"              ->  Index Scan using t11_a_idx on public.t11 t1_2"#)],
                            &[T(r#"                    Output: t1_2.a, t1_2.tableoid, t1_2.ctid"#)],
                            &[T(r#"                    Index Cond: ((t1_2.a > 5) AND (t1_2.a = 8))"#)],
                            &[T(r#"                    Filter: ((ANY (t1_2.a = (hashed SubPlan 2).col1)) AND snoop(t1_2.a) AND leakproof(t1_2.a))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v1;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t1 CASCADE;",
                    expected: Expected::Tag("DROP TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to table t11", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rw_view2;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW rw_view1;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW plain_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE other_tbl;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE base_tbl;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION snoop(anyelement);",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION leakproof(anyelement);",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
