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
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, PRIMARY_KEY_RETURNED, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};


#[test]
fn test_support_functions() {
    run_scripts(&[
        ScriptTest {
            name: "a btree index scan calls a SQL comparison function for its keys, its array keys, and its redundant keys",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(a INT, b INT) RETURNS INT IMMUTABLE LANGUAGE sql AS $$ SELECT CASE WHEN a < b THEN 1 WHEN a > b THEN -1 ELSE 0 END $$;",
                "CREATE OPERATOR CLASS rev_ops FOR TYPE INT USING btree AS OPERATOR 1 >, OPERATOR 2 >=, OPERATOR 3 =, OPERATOR 4 <=, OPERATOR 5 <, FUNCTION 1 rev_cmp(INT, INT);",
                "CREATE TABLE r (a INT);",
                "INSERT INTO r SELECT generate_series(1, 20);",
                "CREATE INDEX r_a ON r (a rev_ops);",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a > 17 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("18")],
                            &[T("19")],
                            &[T("20")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a IN (3, 5, 7) AND a > 4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("7")],
                            &[T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a > 15 AND a > 18;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("20")],
                            &[T("19")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hash partitioning by a SQL hash function routes rows and prunes partitions",
            set_up_script: &[
                "CREATE FUNCTION int_noop_hash(value INT4, seed INT8) RETURNS INT8 IMMUTABLE STRICT LANGUAGE sql AS $$ SELECT value + seed $$;",
                "CREATE OPERATOR CLASS noop_hash_ops FOR TYPE INT4 USING hash AS OPERATOR 1 =, FUNCTION 2 int_noop_hash(INT4, INT8);",
                "CREATE TABLE hp (a INT) PARTITION BY HASH (a noop_hash_ops);",
                "CREATE TABLE hp0 PARTITION OF hp FOR VALUES WITH (MODULUS 2, REMAINDER 0);",
                "CREATE TABLE hp1 PARTITION OF hp FOR VALUES WITH (MODULUS 2, REMAINDER 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO hp VALUES (1), (2), (3), (4);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM hp ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("hp1"), T("1")],
                            &[T("hp0"), T("2")],
                            &[T("hp1"), T("3")],
                            &[T("hp0"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM hp WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
            name: "range and list partitioning by a SQL comparison function order bounds, route rows, and prune partitions",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(a INT, b INT) RETURNS INT IMMUTABLE LANGUAGE sql AS $$ SELECT CASE WHEN a < b THEN 1 WHEN a > b THEN -1 ELSE 0 END $$;",
                "CREATE OPERATOR CLASS rev_ops FOR TYPE INT USING btree AS OPERATOR 1 >, OPERATOR 2 >=, OPERATOR 3 =, OPERATOR 4 <=, OPERATOR 5 <, FUNCTION 1 rev_cmp(INT, INT);",
                "CREATE TABLE rp (a INT) PARTITION BY RANGE (a rev_ops);",
                "CREATE TABLE rp1 PARTITION OF rp FOR VALUES FROM (10) TO (0);",
                "CREATE TABLE rp2 PARTITION OF rp FOR VALUES FROM (20) TO (10);",
                "CREATE TABLE lp (a INT) PARTITION BY LIST (a rev_ops);",
                "CREATE TABLE lp1 PARTITION OF lp FOR VALUES IN (1, 2);",
                "CREATE TABLE lp2 PARTITION OF lp FOR VALUES IN (3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO rp VALUES (5), (15);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM rp ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("rp1"), T("5")],
                            &[T("rp2"), T("15")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM rp WHERE a = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rp3 PARTITION OF rp FOR VALUES FROM (15) TO (12);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "partition \"rp3\" would overlap partition \"rp2\"", position: 51, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lp VALUES (1), (3);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM lp ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("lp1"), T("1")],
                            &[T("lp2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an exclusion constraint checks rows with a SQL operator's function",
            set_up_script: &[
                "CREATE FUNCTION int_eq(a INT, b INT) RETURNS BOOL IMMUTABLE LANGUAGE sql AS $$ SELECT a = b $$;",
                "CREATE OPERATOR === (LEFTARG = INT, RIGHTARG = INT, FUNCTION = int_eq, COMMUTATOR = ===);",
                "CREATE OPERATOR CLASS eq_hash_ops FOR TYPE INT USING hash AS OPERATOR 1 ===, FUNCTION 1 hashint4(INT4);",
                "CREATE TABLE ex (a INT, EXCLUDE USING hash (a eq_hash_ops WITH ===));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO ex VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex VALUES (2);",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"ex_a_excl\"", detail: "Key (a)=(2) conflicts with existing key (a)=(2).", schema: "public", table: "ex", constraint: "ex_a_excl", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a hash index hashes its keys with a SQL hash function",
            set_up_script: &[
                "CREATE FUNCTION int_hash(a INT) RETURNS INT IMMUTABLE LANGUAGE sql AS $$ SELECT a % 3 $$;",
                "CREATE OPERATOR CLASS mod_hash_ops FOR TYPE INT USING hash AS OPERATOR 1 =, FUNCTION 1 int_hash(INT);",
                "CREATE TABLE hx (a INT);",
                "INSERT INTO hx SELECT generate_series(1, 10);",
                "CREATE INDEX hx_a ON hx USING hash (a mod_hash_ops);",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO hx VALUES (11);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM hx WHERE a = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index expressions and predicates calling a PL/pgSQL function with constant arguments are folded",
            set_up_script: &[
                "CREATE FUNCTION half(i INT) RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RETURN i / 2; END $$;",
                "CREATE TABLE ix (id INT PRIMARY KEY, v INT);",
                "INSERT INTO ix SELECT i, i FROM generate_series(1, 10) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX ix_half ON ix (half(v)) WHERE half(4) = 2;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ix VALUES (11, 11);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ix WHERE half(v) = 5 AND half(4) = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("11")],
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
fn test_analyze_evaluates_index_expressions_in_the_session() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE evaluates an index expression with the settings of the session that runs it",
            set_up_script: &[
                "CREATE FUNCTION shifted(i INT) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT i + coalesce(current_setting('idx.shift', true), '0')::INT $$;",
                "CREATE TABLE t (a INT);",
                "INSERT INTO t SELECT i FROM generate_series(1, 100) i;",
                "CREATE INDEX t_shifted ON t (shifted(a));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET idx.shift = 1000;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE t;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, min(v::INT), max(v::INT) FROM pg_stats, unnest(histogram_bounds::TEXT::TEXT[]) v WHERE tablename = 't_shifted' GROUP BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("min", INT4), Column("max", INT4)],
                        rows: &[
                            &[T("shifted"), T("1001"), T("1100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET idx.shift;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE t;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, min(v::INT), max(v::INT) FROM pg_stats, unnest(histogram_bounds::TEXT::TEXT[]) v WHERE tablename = 't_shifted' GROUP BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("min", INT4), Column("max", INT4)],
                        rows: &[
                            &[T("shifted"), T("1001"), T("1100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an index expression evaluated by ANALYZE sees its own progress row",
            set_up_script: &[
                "CREATE FUNCTION progress_phase(i INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RAISE NOTICE 'phase %', (SELECT phase FROM pg_stat_progress_analyze WHERE pid = pg_backend_pid()); RETURN i; END $$;",
                "CREATE TABLE t (a INT);",
                "INSERT INTO t VALUES (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX t_phase ON t (progress_phase(a));",
                    notices: &[Diagnostic { code: "00000", message: "phase <NULL>", ..N }, Diagnostic { code: "00000", message: "phase <NULL>", ..N }],
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE t;",
                    notices: &[Diagnostic { code: "00000", message: "phase computing statistics", ..N }, Diagnostic { code: "00000", message: "phase computing statistics", ..N }],
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_builtin_support_functions_reaching_sql_functions() {
    run_scripts(&[
        ScriptTest {
            name: "sorts, equality, containment, hash aggregation, and hash joins over arrays and composites of a type whose btree and hash classes call SQL functions",
            set_up_script: &[
                "CREATE FUNCTION box_area_cmp(BOX, BOX) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN area($1) < area($2) THEN -1 WHEN area($1) > area($2) THEN 1 ELSE 0 END $$;",
                "CREATE FUNCTION box_area_lt(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) < area($2) $$;",
                "CREATE FUNCTION box_area_le(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) <= area($2) $$;",
                "CREATE FUNCTION box_area_eq(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) = area($2) $$;",
                "CREATE FUNCTION box_area_ge(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) >= area($2) $$;",
                "CREATE FUNCTION box_area_gt(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) > area($2) $$;",
                "CREATE OPERATOR <<< (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_lt);",
                "CREATE OPERATOR <<<= (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_le);",
                "CREATE OPERATOR === (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_eq, HASHES);",
                "CREATE OPERATOR >>>= (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_ge);",
                "CREATE OPERATOR >>> (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_gt);",
                "CREATE OPERATOR CLASS box_area_ops DEFAULT FOR TYPE BOX USING btree AS OPERATOR 1 <<<, OPERATOR 2 <<<=, OPERATOR 3 ===, OPERATOR 4 >>>=, OPERATOR 5 >>>, FUNCTION 1 box_area_cmp(BOX, BOX);",
                "CREATE FUNCTION box_area_hash(BOX) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT hashfloat8(area($1)) $$;",
                "CREATE OPERATOR CLASS box_area_hash_ops DEFAULT FOR TYPE BOX USING hash AS OPERATOR 1 ===, FUNCTION 1 box_area_hash(BOX);",
                "CREATE TYPE boxed AS (b BOX, i INT);",
                "CREATE TABLE ba (id INT PRIMARY KEY, a BOX[], c boxed);",
                "INSERT INTO ba VALUES (1, ARRAY['(0,0),(3,3)']::BOX[], ROW('(0,0),(3,3)', 1)), (2, ARRAY['(0,0),(1,1)', '(0,0),(5,5)']::BOX[], ROW('(0,0),(1,1)', 2)), (3, ARRAY['(1,1),(2,2)']::BOX[], ROW('(5,5),(6,6)', 0));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id FROM ba ORDER BY a, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
                    query: "SELECT id FROM ba ORDER BY c, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
                    query: "SELECT id FROM ba WHERE a = ARRAY['(5,5),(6,6)']::BOX[];",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ba WHERE c = ROW('(1,1),(2,2)', 0)::boxed;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_position(ARRAY['(0,0),(1,1)', '(0,0),(2,2)']::BOX[], '(5,5),(7,7)'::BOX);",
                    expected: Expected::Rows {
                        columns: &[Column("array_position", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY['(0,0),(2,2)']::BOX[] @> ARRAY['(1,1),(3,3)']::BOX[];",
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
                    query: "SET enable_sort = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM ba GROUP BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate")],
                            &[T("  Group Key: a")],
                            &[T("  ->  Seq Scan on ba")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ba GROUP BY a ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                            &[T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ba GROUP BY c ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                            &[T("1")],
                        ],
                        tag: "SELECT 3",
                    },
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
                    query: "EXPLAIN (COSTS OFF) SELECT x.id, y.id FROM ba x JOIN ba y ON x.a = y.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Hash Join")],
                            &[T("  Hash Cond: (x.a = y.a)")],
                            &[T("  ->  Seq Scan on ba x")],
                            &[T("  ->  Hash")],
                            &[T("        ->  Seq Scan on ba y")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x.id, y.id FROM ba x JOIN ba y ON x.a = y.a ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
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
                    query: "SELECT x.id, y.id FROM ba x JOIN ba y ON x.c = y.c ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "btree indexes over arrays and composites of a type whose default btree class compares by a SQL function",
            set_up_script: &[
                "CREATE FUNCTION box_area_cmp(BOX, BOX) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN area($1) < area($2) THEN -1 WHEN area($1) > area($2) THEN 1 ELSE 0 END $$;",
                "CREATE FUNCTION box_area_lt(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) < area($2) $$;",
                "CREATE FUNCTION box_area_le(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) <= area($2) $$;",
                "CREATE FUNCTION box_area_eq(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) = area($2) $$;",
                "CREATE FUNCTION box_area_ge(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) >= area($2) $$;",
                "CREATE FUNCTION box_area_gt(BOX, BOX) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT area($1) > area($2) $$;",
                "CREATE OPERATOR <<< (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_lt);",
                "CREATE OPERATOR <<<= (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_le);",
                "CREATE OPERATOR === (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_eq, HASHES);",
                "CREATE OPERATOR >>>= (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_ge);",
                "CREATE OPERATOR >>> (LEFTARG = BOX, RIGHTARG = BOX, FUNCTION = box_area_gt);",
                "CREATE OPERATOR CLASS box_area_ops DEFAULT FOR TYPE BOX USING btree AS OPERATOR 1 <<<, OPERATOR 2 <<<=, OPERATOR 3 ===, OPERATOR 4 >>>=, OPERATOR 5 >>>, FUNCTION 1 box_area_cmp(BOX, BOX);",
                "CREATE FUNCTION box_area_hash(BOX) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT hashfloat8(area($1)) $$;",
                "CREATE OPERATOR CLASS box_area_hash_ops DEFAULT FOR TYPE BOX USING hash AS OPERATOR 1 ===, FUNCTION 1 box_area_hash(BOX);",
                "CREATE TYPE boxed AS (b BOX, i INT);",
                "CREATE TABLE ba (id INT PRIMARY KEY, a BOX[], c boxed);",
                "INSERT INTO ba VALUES (1, ARRAY['(0,0),(3,3)']::BOX[], ROW('(0,0),(3,3)', 1)), (2, ARRAY['(0,0),(1,1)', '(0,0),(5,5)']::BOX[], ROW('(0,0),(1,1)', 2)), (3, ARRAY['(1,1),(2,2)']::BOX[], ROW('(5,5),(6,6)', 0));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX ba_a ON ba (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ba_c ON ba (c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ba ORDER BY a, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
                    query: "SELECT id FROM ba ORDER BY c, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
                    query: "SELECT id FROM ba WHERE a = ARRAY['(5,5),(6,6)']::BOX[];",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ba WHERE c = ROW('(1,1),(2,2)', 0)::boxed;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX ba_a_u ON ba (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ba VALUES (4, ARRAY['(7,7),(8,8)']::BOX[], NULL);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"ba_a_u\"", detail: "Key (a)=({(8,8),(7,7)}) already exists.", schema: "public", table: "ba", constraint: "ba_a_u", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a btree index over a range type whose subtype operator class compares by a SQL function",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT btint4cmp($2, $1) $$;",
                "CREATE OPERATOR CLASS int4_rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 > (INT4, INT4), OPERATOR 2 >= (INT4, INT4), OPERATOR 3 = (INT4, INT4), OPERATOR 4 <= (INT4, INT4), OPERATOR 5 < (INT4, INT4), FUNCTION 1 rev_cmp(INT4, INT4);",
                "CREATE TYPE revrange AS RANGE (SUBTYPE = INT4, SUBTYPE_OPCLASS = int4_rev_ops);",
                "CREATE TABLE rr (id INT PRIMARY KEY, v revrange);",
                "INSERT INTO rr VALUES (1, revrange(5, 1)), (2, revrange(9, 7)), (3, revrange(5, 3)), (4, revrange(2, 0));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX rr_v ON rr (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX rr_v_u ON rr (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM rr ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("2"), T("[9,7)")],
                            &[T("3"), T("[5,3)")],
                            &[T("1"), T("[5,1)")],
                            &[T("4"), T("[2,0)")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rr WHERE v = revrange(5, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rr WHERE v < revrange(5, 1) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rr VALUES (5, revrange(9, 7));",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"rr_v_u\"", detail: "Key (v)=([9,7)) already exists.", schema: "public", table: "rr", constraint: "rr_v_u", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_literal_input_in_the_session() {
    run_scripts(&[
        ScriptTest {
            name: "literals whose input functions call SQL functions, in queries, defaults, CHECK constraints, domains, an aggregate initial value, USING, SET EXPRESSION, and a partition key",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT btint4cmp($2, $1) $$;",
                "CREATE OPERATOR CLASS int4_rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 > (INT4, INT4), OPERATOR 2 >= (INT4, INT4), OPERATOR 3 = (INT4, INT4), OPERATOR 4 <= (INT4, INT4), OPERATOR 5 < (INT4, INT4), FUNCTION 1 rev_cmp(INT4, INT4);",
                "CREATE TYPE revrange AS RANGE (SUBTYPE = INT4, SUBTYPE_OPCLASS = int4_rev_ops);",
                "CREATE FUNCTION is_even(INT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT $1 % 2 = 0 $$;",
                "CREATE DOMAIN even AS INT CHECK (is_even(VALUE));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT '[5,1)'::revrange, '4'::even;",
                    expected: Expected::Rows {
                        columns: &[Column("revrange", USER_DEFINED), Column("even", INT4)],
                        rows: &[
                            &[T("[5,1)"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,5)'::revrange;",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "range lower bound must be less than or equal to range upper bound", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '3'::even;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain even violates check constraint \"even_check\"", schema: "public", data_type: "even", constraint: "even_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (id INT PRIMARY KEY, r revrange DEFAULT '[9,3)', e even DEFAULT '2', CHECK (r <> '[2,1)'), CHECK (e <> '8'));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 (id) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 (id, e) VALUES (2, '8');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t1\" violates check constraint \"t1_e_check\"", detail: "Failing row contains (2, [9,3), 8).", schema: "public", table: "t1", constraint: "t1_e_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ADD COLUMN r2 revrange DEFAULT '[7,6)';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ADD CONSTRAINT c3 CHECK (r2 <> '[1,0)');",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN e SET DEFAULT '6';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ADD COLUMN x INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN x TYPE even USING '10';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 (id) VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("r", USER_DEFINED), Column("e", INT4), Column("r2", USER_DEFINED), Column("x", INT4)],
                        rows: &[
                            &[T("1"), T("[9,3)"), T("2"), T("[7,6)"), T("10")],
                            &[T("3"), T("[9,3)"), T("6"), T("[7,6)"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN revdom AS revrange DEFAULT '[3,2)' CHECK (VALUE <> '[5,4)');",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id INT, d revdom);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (id) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN revdom SET DEFAULT '[8,7)';",
                    expected: Expected::Tag("ALTER DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER DOMAIN revdom ADD CONSTRAINT not_six CHECK (VALUE <> '[7,6)');",
                    expected: Expected::Tag("ALTER DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (id) VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[3,2)")],
                            &[T("2"), T("[8,7)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE AGGREGATE rev_merge(revrange) (SFUNC = range_merge, STYPE = revrange, INITCOND = '[1,0)');",
                    expected: Expected::Tag("CREATE AGGREGATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rev_merge(r) FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("rev_merge", USER_DEFINED)],
                        rows: &[
                            &[T("[9,0)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (a INT, g revrange GENERATED ALWAYS AS (revrange(a, 0)) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3 (a) VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t3 ALTER COLUMN g SET EXPRESSION AS (revrange(a + 1, 0) * '[100,0)');",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("g", USER_DEFINED)],
                        rows: &[
                            &[T("5"), T("[6,0)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p (e even) PARTITION BY RANGE ((e + '2'::even));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (100);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES ('4');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("e", INT4)],
                        rows: &[
                            &[T("4")],
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
fn test_btree_indexes_over_arrays_and_records_of_comparator_ordered_types() {
    run_scripts(&[ScriptTest {
        name: "btree indexes over arrays and records of a range whose subtype compares by SQL, and over an ICU text array",
        set_up_script: &[
            "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT btint4cmp($2, $1) $$;",
            "CREATE OPERATOR CLASS int4_rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 > (INT4, INT4), OPERATOR 2 >= (INT4, INT4), OPERATOR 3 = (INT4, INT4), OPERATOR 4 <= (INT4, INT4), OPERATOR 5 < (INT4, INT4), FUNCTION 1 rev_cmp(INT4, INT4);",
            "CREATE TYPE revrange AS RANGE (SUBTYPE = INT4, SUBTYPE_OPCLASS = int4_rev_ops);",
            "CREATE TYPE revpair AS (r revrange, n INT);",
            "CREATE TABLE ra (id INT PRIMARY KEY, a revrange[], p revpair);",
            "INSERT INTO ra VALUES (1, ARRAY[revrange(5, 1)], ROW(revrange(5, 1), 1)), (2, ARRAY[revrange(9, 7), revrange(2, 0)], ROW(revrange(9, 7), 2)), (3, ARRAY[revrange(5, 3)], ROW(revrange(5, 1), 0));",
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "CREATE INDEX ra_a ON ra (a);",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE UNIQUE INDEX ra_p ON ra (p);",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "SET enable_seqscan = off;",
                expected: Expected::Tag("SET"),
                ..A
            },
            ScriptTestAssertion {
                query: "EXPLAIN (COSTS OFF) SELECT id FROM ra ORDER BY a;",
                expected: Expected::Rows {
                    columns: &[Column("QUERY PLAN", TEXT)],
                    rows: &[
                        &[T("Index Scan using ra_a on ra")],
                    ],
                    tag: "EXPLAIN",
                },
                diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan using ra_a on ra"])),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id FROM ra ORDER BY a;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("2")],
                        &[T("3")],
                        &[T("1")],
                    ],
                    tag: "SELECT 3",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id FROM ra ORDER BY p;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("2")],
                        &[T("3")],
                        &[T("1")],
                    ],
                    tag: "SELECT 3",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id FROM ra WHERE a = ARRAY[revrange(5, 3)];",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id FROM ra WHERE p > ROW(revrange(5, 1), 0)::revpair ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("1")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "INSERT INTO ra VALUES (4, ARRAY[revrange(1, 0)], ROW(revrange(9, 7), 2));",
                expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"ra_p\"", detail: "Key (p)=((\"[9,7)\",2)) already exists.", schema: "public", table: "ra", constraint: "ra_p", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "INSERT INTO ra VALUES (5, ARRAY[revrange(1, 0)], ROW(revrange(9, 7), 3));",
                expected: Expected::Tag("INSERT 0 1"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id FROM ra ORDER BY a, id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("2")],
                        &[T("3")],
                        &[T("1")],
                        &[T("5")],
                    ],
                    tag: "SELECT 4",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TABLE ti (a TEXT[] COLLATE \"und-x-icu\" PRIMARY KEY, n INT);",
                expected: Expected::Tag("CREATE TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "INSERT INTO ti VALUES (ARRAY['b'], 1), (ARRAY['B'], 2), (ARRAY['a', 'C'], 3), (ARRAY['a', 'b'], 4), (ARRAY['A'], 5);",
                expected: Expected::Tag("INSERT 0 5"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT n FROM ti ORDER BY a;",
                expected: Expected::Rows {
                    columns: &[Column("n", INT4)],
                    rows: &[
                        &[T("4")],
                        &[T("3")],
                        &[T("5")],
                        &[T("1")],
                        &[T("2")],
                    ],
                    tag: "SELECT 5",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT n FROM ti WHERE a > ARRAY['a'] COLLATE \"und-x-icu\" ORDER BY a;",
                expected: Expected::Rows {
                    columns: &[Column("n", INT4)],
                    rows: &[
                        &[T("4")],
                        &[T("3")],
                        &[T("5")],
                        &[T("1")],
                        &[T("2")],
                    ],
                    tag: "SELECT 5",
                },
                ..A
            },
        ],
        ..S
    }]);
}
