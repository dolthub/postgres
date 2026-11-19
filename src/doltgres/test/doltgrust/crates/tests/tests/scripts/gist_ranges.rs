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
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};


#[test]
fn test_gist_ranges() {
    run_scripts(&[
        ScriptTest {
            name: "range_ops searches of an int4range column, with empty, infinite, and NULL ranges",
            set_up_script: &[
                "CREATE TABLE r (id INT PRIMARY KEY, v INT4RANGE);",
                "INSERT INTO r SELECT i, int4range(i, i + (i % 7) + 1) FROM generate_series(1, 2000) i;",
                "INSERT INTO r VALUES (3001, 'empty'), (3002, NULL), (3003, '(,5)'), (3004, '[1990,)'), (3005, 'empty');",
                "CREATE INDEX r_v ON r USING gist (v);",
                "VACUUM r;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM r WHERE v = 'empty';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on r")],
                            &[T("  Recheck Cond: (v = 'empty'::int4range)")],
                            &[T("  ->  Bitmap Index Scan on r_v")],
                            &[T("        Index Cond: (v = 'empty'::int4range)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE v = 'empty' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3001")],
                            &[T("3005")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM r WHERE v = int4range(1000, 1007);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4RANGE)],
                        rows: &[
                            &[T("1000"), T("[1000,1007)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT v FROM r WHERE v -|- int4range(500, 600);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using r_v on r")],
                            &[T("  Index Cond: (v -|- '[500,600)'::int4range)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM r WHERE v -|- int4range(500, 600) ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4RANGE)],
                        rows: &[
                            &[T("[498,500)")],
                            &[T("[600,606)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE v @> 1000 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("997")],
                            &[T("998")],
                            &[T("999")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM r WHERE v <@ int4range(100, 200);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("99")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM r WHERE v << int4range(10, 20);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM r WHERE v >> int4range(1980, 1990);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM r WHERE v &< int4range(10, 20);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("17")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM r WHERE v &> int4range(1980, 1990);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("22")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM r WHERE v @> int4range(1000, 1002);",
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
                    query: "SELECT count(*) FROM r WHERE v && int4range(1000, 1002);",
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
                    query: "SELECT count(*) FROM r WHERE v <@ 'empty'::int4range;",
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
                    query: "SELECT count(*) FROM r WHERE v && 'empty'::int4range;",
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
                    query: "INSERT INTO r VALUES (4000, '[5000,5010)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r SET v = '[6000,6001)' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r WHERE id = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM r WHERE v = '[6000,6001)' OR v = '[5000,5010)' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4RANGE)],
                        rows: &[
                            &[T("1"), T("[6000,6001)")],
                            &[T("4000"), T("[5000,5010)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE v = '[2,4)';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "exclusion constraints over range_ops, with numrange display scales, empty and infinite ranges, and two range columns",
            set_up_script: &[
                "SET TIME ZONE 'UTC';",
                "CREATE TABLE b (id INT PRIMARY KEY, v NUMRANGE, EXCLUDE USING gist (v WITH &&));",
                "INSERT INTO b SELECT i, numrange(10 * i, 10 * i + 5) FROM generate_series(1, 300) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (5000, numrange(1003, 1004));",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"b_v_excl\"", detail: "Key (v)=([1003,1004)) conflicts with existing key (v)=([1000,1005)).", schema: "public", table: "b", constraint: "b_v_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (5001, numrange(1005, 1010));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (5002, 'empty'), (5003, 'empty');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (5004, '(,11)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"b_v_excl\"", detail: "Key (v)=((,11)) conflicts with existing key (v)=([10,15)).", schema: "public", table: "b", constraint: "b_v_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (5005, '(,10)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (5006, '[3006,)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (5007, '[3005.0,)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"b_v_excl\"", detail: "Key (v)=([3005.0,)) conflicts with existing key (v)=([3006,)).", schema: "public", table: "b", constraint: "b_v_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (5008, '[1005.00,1005.000]');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"b_v_excl\"", detail: "Key (v)=([1005.00,1005.000]) conflicts with existing key (v)=([1005,1010)).", schema: "public", table: "b", constraint: "b_v_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE b SET v = numrange(15, 21) WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"b_v_excl\"", detail: "Key (v)=([15,21)) conflicts with existing key (v)=([20,25)).", schema: "public", table: "b", constraint: "b_v_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE b SET v = numrange(15, 20) WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM b;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("305")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE d (id INT PRIMARY KEY, v DATERANGE, w TSTZRANGE, EXCLUDE USING gist (v WITH =, w WITH &&));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (1, '[2026-01-01,2026-02-01)', '[2026-01-01 00:00+00,2026-01-02 00:00+00)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (2, '[2026-01-01,2026-02-01)', '[2026-01-01 12:00+00,2026-01-03 00:00+00)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"d_v_w_excl\"", detail: "Key (v, w)=([2026-01-01,2026-02-01), [\"2026-01-01 12:00:00+00\",\"2026-01-03 00:00:00+00\")) conflicts with existing key (v, w)=([2026-01-01,2026-02-01), [\"2026-01-01 00:00:00+00\",\"2026-01-02 00:00:00+00\")).", schema: "public", table: "d", constraint: "d_v_w_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (3, '[2026-01-01,2026-02-01)', '[2026-01-02 00:00+00,2026-01-03 00:00+00)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (4, '[2026-01-01,2026-01-31]', '[2026-01-02 00:00+00,2026-01-03 00:00+00)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"d_v_w_excl\"", detail: "Key (v, w)=([2026-01-01,2026-02-01), [\"2026-01-02 00:00:00+00\",\"2026-01-03 00:00:00+00\")) conflicts with existing key (v, w)=([2026-01-01,2026-02-01), [\"2026-01-02 00:00:00+00\",\"2026-01-03 00:00:00+00\")).", schema: "public", table: "d", constraint: "d_v_w_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (5, '[2026-01-02,2026-02-01)', '[2026-01-02 00:00+00,2026-01-03 00:00+00)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "range_ops over subtypes whose keys are not byte-ordered, and over an ICU collation",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT btint4cmp($2, $1) $$;",
                "CREATE OPERATOR CLASS int4_rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 > (INT4, INT4), OPERATOR 2 >= (INT4, INT4), OPERATOR 3 = (INT4, INT4), OPERATOR 4 <= (INT4, INT4), OPERATOR 5 < (INT4, INT4), FUNCTION 1 rev_cmp(INT4, INT4);",
                "CREATE TYPE revrange AS RANGE (SUBTYPE = INT4, SUBTYPE_OPCLASS = int4_rev_ops);",
                "CREATE TABLE rr (id INT PRIMARY KEY, v revrange);",
                "CREATE FUNCTION box_area_cmp(BOX, BOX) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN area($1) < area($2) THEN -1 WHEN area($1) > area($2) THEN 1 ELSE 0 END $$;",
                "CREATE OPERATOR CLASS box_area_ops DEFAULT FOR TYPE BOX USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 box_area_cmp(BOX, BOX);",
                "CREATE TYPE boxrange AS RANGE (SUBTYPE = BOX);",
                "CREATE TABLE br (id INT PRIMARY KEY, v boxrange);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO rr VALUES (1, revrange(5, 1)), (2, revrange(9, 7));",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX rr_v ON rr USING gist (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO br VALUES (1, boxrange('(0,0),(1,1)', '(0,0),(2,2)'));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX br_v ON br USING gist (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE textrange_ci AS RANGE (SUBTYPE = TEXT, COLLATION = \"und-x-icu\");",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tr (id INT PRIMARY KEY, v textrange_ci, EXCLUDE USING gist (v WITH &&));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES (1, textrange_ci('a', 'b')), (2, textrange_ci('B', 'C'));",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES (3, textrange_ci('ab', 'ac'));",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"tr_v_excl\"", detail: "Key (v)=([ab,ac)) conflicts with existing key (v)=([a,b)).", schema: "public", table: "tr", constraint: "tr_v_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES (4, textrange_ci('b', 'B'));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM tr WHERE v = textrange_ci('a', 'b');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[a,b)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM tr WHERE v -|- textrange_ci('b', 'ba') ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[a,b)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multirange_ops, and range_ops with multirange queries",
            set_up_script: &[
                "CREATE TABLE m (id INT PRIMARY KEY, v int4multirange);",
                "INSERT INTO m SELECT g, CASE WHEN g % 10 = 0 THEN '{}' ELSE int4multirange(int4range(g, g + 3), int4range(g + 5, g + 7)) END FROM generate_series(1, 300) g;",
                "CREATE INDEX m_v ON m USING gist (v);",
                "VACUUM ANALYZE m;",
                "CREATE TABLE r (id INT PRIMARY KEY, v int4range);",
                "INSERT INTO r SELECT g, CASE WHEN g % 10 = 0 THEN 'empty' ELSE int4range(g, g + 4) END FROM generate_series(1, 300) g;",
                "CREATE INDEX r_v ON r USING gist (v);",
                "VACUUM ANALYZE r;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM m WHERE v && int4range(10, 12);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on m")],
                            &[T("  Recheck Cond: (v && '[10,12)'::int4range)")],
                            &[T("  ->  Bitmap Index Scan on m_v")],
                            &[T("        Index Cond: (v && '[10,12)'::int4range)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v && int4range(10, 12) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("8")],
                            &[T("9")],
                            &[T("11")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v && '{[10,12), [200,201)}'::int4multirange ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("8")],
                            &[T("9")],
                            &[T("11")],
                            &[T("194")],
                            &[T("195")],
                            &[T("198")],
                            &[T("199")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v @> 9 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v @> int4range(9, 10) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v @> '{[9,10), [12,13)}'::int4multirange ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v <@ int4range(5, 20) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                            &[T("10")],
                            &[T("11")],
                            &[T("12")],
                            &[T("13")],
                            &[T("20")],
                            &[T("30")],
                            &[T("40")],
                            &[T("50")],
                            &[T("60")],
                            &[T("70")],
                            &[T("80")],
                            &[T("90")],
                            &[T("100")],
                            &[T("110")],
                            &[T("120")],
                            &[T("130")],
                            &[T("140")],
                            &[T("150")],
                            &[T("160")],
                            &[T("170")],
                            &[T("180")],
                            &[T("190")],
                            &[T("200")],
                            &[T("210")],
                            &[T("220")],
                            &[T("230")],
                            &[T("240")],
                            &[T("250")],
                            &[T("260")],
                            &[T("270")],
                            &[T("280")],
                            &[T("290")],
                            &[T("300")],
                        ],
                        tag: "SELECT 38",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM m WHERE v <@ '{}'::int4multirange;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM m WHERE v = '{}'::int4multirange;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM m WHERE v = '{[7,10), [12,14)}'::int4multirange;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using m_v on m")],
                            &[T("  Index Cond: (v = '{[7,10),[12,14)}'::int4multirange)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v = '{[7,10), [12,14)}'::int4multirange;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v = '{[7,14)}'::int4multirange;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v << int4range(10, 12) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
                    query: "SELECT id FROM m WHERE v >> '{[280,281)}'::int4multirange ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("281")],
                            &[T("282")],
                            &[T("283")],
                            &[T("284")],
                            &[T("285")],
                            &[T("286")],
                            &[T("287")],
                            &[T("288")],
                            &[T("289")],
                            &[T("291")],
                            &[T("292")],
                            &[T("293")],
                            &[T("294")],
                            &[T("295")],
                            &[T("296")],
                            &[T("297")],
                            &[T("298")],
                            &[T("299")],
                        ],
                        tag: "SELECT 18",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v &< int4range(10, 12) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
                    query: "SELECT count(*) FROM m WHERE v &> int4range(10, 12);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("261")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM m WHERE v -|- int4range(1, 3) ORDER BY id;",
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
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM r WHERE v && '{[10,12), [100,101)}'::int4multirange;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on r")],
                            &[T("  Recheck Cond: (v && '{[10,12),[100,101)}'::int4multirange)")],
                            &[T("  ->  Bitmap Index Scan on r_v")],
                            &[T("        Index Cond: (v && '{[10,12),[100,101)}'::int4multirange)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE v && '{[10,12), [100,101)}'::int4multirange ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                            &[T("11")],
                            &[T("97")],
                            &[T("98")],
                            &[T("99")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE v @> '{[10,12)}'::int4multirange ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE v <@ '{[10,20), [30,35)}'::int4multirange ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("11")],
                            &[T("12")],
                            &[T("13")],
                            &[T("14")],
                            &[T("15")],
                            &[T("16")],
                            &[T("20")],
                            &[T("30")],
                            &[T("31")],
                            &[T("40")],
                            &[T("50")],
                            &[T("60")],
                            &[T("70")],
                            &[T("80")],
                            &[T("90")],
                            &[T("100")],
                            &[T("110")],
                            &[T("120")],
                            &[T("130")],
                            &[T("140")],
                            &[T("150")],
                            &[T("160")],
                            &[T("170")],
                            &[T("180")],
                            &[T("190")],
                            &[T("200")],
                            &[T("210")],
                            &[T("220")],
                            &[T("230")],
                            &[T("240")],
                            &[T("250")],
                            &[T("260")],
                            &[T("270")],
                            &[T("280")],
                            &[T("290")],
                            &[T("300")],
                        ],
                        tag: "SELECT 37",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE v << '{[10,12)}'::int4multirange ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM r WHERE v &> '{[10,12)}'::int4multirange;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("261")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE v -|- '{[10,12)}'::int4multirange ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("6")],
                            &[T("12")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE x (id INT PRIMARY KEY, v datemultirange, EXCLUDE USING gist (v WITH &&));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (1, '{[2026-01-01,2026-01-05), [2026-02-01,2026-02-05)}'), (2, '{[2026-01-10,2026-01-20)}');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (3, '{[2026-01-06,2026-01-08)}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (4, '{[2026-01-04,2026-01-06)}');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"x_v_excl\"", detail: "Key (v)=({[2026-01-04,2026-01-06)}) conflicts with existing key (v)=({[2026-01-01,2026-01-05),[2026-02-01,2026-02-05)}).", schema: "public", table: "x", constraint: "x_v_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (5, '{}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO x VALUES (6, '{}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN dm AS int4multirange CHECK (VALUE <> '{}');",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE d (id INT PRIMARY KEY, v dm);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (1, '{[1,3), [8,9)}'), (2, '{[4,6)}');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX d_v ON d USING gist (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM d WHERE v && int4range(5, 8) ORDER BY id;",
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
                    query: "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT btint4cmp($2, $1) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS int4_rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 > (INT4, INT4), OPERATOR 2 >= (INT4, INT4), OPERATOR 3 = (INT4, INT4), OPERATOR 4 <= (INT4, INT4), OPERATOR 5 < (INT4, INT4), FUNCTION 1 rev_cmp(INT4, INT4);",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE revrange AS RANGE (SUBTYPE = INT4, SUBTYPE_OPCLASS = int4_rev_ops);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rm (id INT PRIMARY KEY, v revmultirange);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rm VALUES (1, revmultirange(revrange(5, 1), revrange(9, 7)));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX rm_v ON rm USING gist (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a GiST index scan returns every row of a deferrable primary key's repeated key",
            set_up_script: &[
                "CREATE TABLE gd (id INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, r INT4RANGE);",
                "CREATE INDEX gd_r ON gd USING gist (r);",
                "INSERT INTO gd VALUES (1, '[1,5)');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO gd VALUES (1, '[2,6)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM gd WHERE r && '[3,4)' ORDER BY r;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("r", INT4RANGE)],
                        rows: &[
                            &[T("1"), T("[1,5)")],
                            &[T("1"), T("[2,6)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r FROM gd WHERE r && '[3,4)' ORDER BY r;",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4RANGE)],
                        rows: &[
                            &[T("[1,5)")],
                            &[T("[2,6)")],
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
                    query: "SELECT * FROM gd ORDER BY r;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("r", INT4RANGE)],
                        rows: &[
                            &[T("1"), T("[1,5)")],
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
fn test_without_overlaps() {
    run_scripts(&[
        ScriptTest {
            name: "a WITHOUT OVERLAPS primary key is a GiST exclusion constraint that rejects overlapping, empty, and NULL periods",
            set_up_script: &[
                "CREATE TABLE wo (id INT4RANGE, valid_at DATERANGE, note TEXT, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT conname, contype, conperiod, conexclop::regoper[], pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'wo'::regclass ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("contype", CHAR), Column("conperiod", BOOL), Column("conexclop", REGOPER_ARRAY), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("wo_id_not_null"), T("n"), T("f"), Null, T("NOT NULL id")],
                            &[T("wo_pkey"), T("p"), T("t"), T("{pg_catalog.=,pg_catalog.&&}"), T("PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)")],
                            &[T("wo_valid_at_not_null"), T("n"), T("f"), Null, T("NOT NULL valid_at")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT i.indexrelid::regclass, am.amname, i.indisunique, i.indisprimary, i.indisexclusion, i.indimmediate, i.indnatts, i.indkey FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am am ON am.oid = c.relam WHERE i.indrelid = 'wo'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("amname", NAME), Column("indisunique", BOOL), Column("indisprimary", BOOL), Column("indisexclusion", BOOL), Column("indimmediate", BOOL), Column("indnatts", INT2), Column("indkey", INT2VECTOR)],
                        rows: &[
                            &[T("wo_pkey"), T("gist"), T("t"), T("t"), T("t"), T("t"), T("2"), T("1 2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES ('[1,2)', '[2020-01-01,2020-02-01)', 'a'), ('[1,2)', '[2020-02-01,2020-03-01)', 'b'), ('[2,3)', '[2020-01-15,2020-02-15)', 'c');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES ('[1,2)', '[2020-01-15,2020-02-15)', 'd');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wo_pkey\"", detail: "Key (id, valid_at)=([1,2), [2020-01-15,2020-02-15)) conflicts with existing key (id, valid_at)=([1,2), [2020-01-01,2020-02-01)).", schema: "public", table: "wo", constraint: "wo_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES ('[1,2)', 'empty', 'e');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "empty WITHOUT OVERLAPS value found in column \"valid_at\" in relation \"wo\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES ('[1,2)', NULL, 'f');",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"valid_at\" of relation \"wo\" violates not-null constraint", detail: "Failing row contains ([1,2), null, f).", schema: "public", table: "wo", column: "valid_at", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES ('[1,2)', '[2020-01-01,2020-02-01)', 'g');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wo_pkey\"", detail: "Key (id, valid_at)=([1,2), [2020-01-01,2020-02-01)) conflicts with existing key (id, valid_at)=([1,2), [2020-01-01,2020-02-01)).", schema: "public", table: "wo", constraint: "wo_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE wo SET valid_at = '[2020-01-20,2020-02-10)' WHERE note = 'c';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE wo SET id = '[2,3)' WHERE note = 'a';",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wo_pkey\"", detail: "Key (id, valid_at)=([2,3), [2020-01-01,2020-02-01)) conflicts with existing key (id, valid_at)=([2,3), [2020-01-20,2020-02-10)).", schema: "public", table: "wo", constraint: "wo_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES ('[1,2)', '[2020-01-15,2020-02-15)', 'h') ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES ('[5,6)', '[2020-01-15,2020-02-15)', 'i') ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wo VALUES ('[5,6)', '[2020-01-15,2020-02-15)', 'j') ON CONFLICT (id, valid_at) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "there is no unique or exclusion constraint matching the ON CONFLICT specification", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wo ORDER BY id, valid_at;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("valid_at", DATERANGE), Column("note", TEXT)],
                        rows: &[
                            &[T("[1,2)"), T("[2020-01-01,2020-02-01)"), T("a")],
                            &[T("[1,2)"), T("[2020-02-01,2020-03-01)"), T("b")],
                            &[T("[2,3)"), T("[2020-01-20,2020-02-10)"), T("c")],
                            &[T("[5,6)"), T("[2020-01-15,2020-02-15)"), T("i")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE won (id NUMRANGE, valid_at DATERANGE, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO won VALUES ('[1.0,2)', '[2020-01-01,2020-02-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO won VALUES ('[1.00,2)', '[2020-01-01,2020-02-01)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"won_pkey\"", detail: "Key (id, valid_at)=([1.00,2), [2020-01-01,2020-02-01)) conflicts with existing key (id, valid_at)=([1.0,2), [2020-01-01,2020-02-01)).", schema: "public", table: "won", constraint: "won_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO won VALUES ('[1.00,2)', '[2020-03-01,2020-04-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM won ORDER BY valid_at;",
                    expected: Expected::Rows {
                        columns: &[Column("id", NUMRANGE), Column("valid_at", DATERANGE)],
                        rows: &[
                            &[T("[1.0,2)"), T("[2020-01-01,2020-02-01)")],
                            &[T("[1.00,2)"), T("[2020-03-01,2020-04-01)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE wr (id INT4RANGE, valid_at DATERANGE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wr SELECT int4range(g, g + 1), daterange('2020-01-01', '2020-02-01') FROM generate_series(1, 50) g;",
                    expected: Expected::Tag("INSERT 0 50"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE wr ADD PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relpages > 0 AS has_pages, reltuples FROM pg_class WHERE relname IN ('wr', 'wr_pkey') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("has_pages", BOOL), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("wr"), T("t"), T("50")],
                            &[T("wr_pkey"), T("t"), T("50")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE wo, won, wr;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a deferrable WITHOUT OVERLAPS primary key checks its periods at commit and at SET CONSTRAINTS, repeated keys included",
            set_up_script: &[
                "CREATE TABLE wd (id INT4RANGE, valid_at DATERANGE, note TEXT, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS) DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT conname, condeferrable, condeferred, conperiod FROM pg_constraint WHERE conrelid = 'wd'::regclass AND contype = 'p';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("condeferrable", BOOL), Column("condeferred", BOOL), Column("conperiod", BOOL)],
                        rows: &[
                            &[T("wd_pkey"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indimmediate FROM pg_index WHERE indrelid = 'wd'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indimmediate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wd VALUES ('[1,2)', '[2020-01-01,2020-02-01)', 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wd VALUES ('[1,2)', '[2020-01-15,2020-02-15)', 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wd VALUES ('[1,2)', '[2020-01-01,2020-02-01)', 'c');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wd ORDER BY valid_at, note;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("valid_at", DATERANGE), Column("note", TEXT)],
                        rows: &[
                            &[T("[1,2)"), T("[2020-01-01,2020-02-01)"), T("a")],
                            &[T("[1,2)"), T("[2020-01-01,2020-02-01)"), T("c")],
                            &[T("[1,2)"), T("[2020-01-15,2020-02-15)"), T("b")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wd_pkey\"", detail: "Key (id, valid_at)=([1,2), [2020-01-15,2020-02-15)) conflicts with existing key (id, valid_at)=([1,2), [2020-01-01,2020-02-01)).", schema: "public", table: "wd", constraint: "wd_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wd VALUES ('[1,2)', '[2020-01-01,2020-02-01)', 'd');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE wd SET valid_at = '[2020-03-01,2020-04-01)' WHERE note = 'a';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wd ORDER BY valid_at, note;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("valid_at", DATERANGE), Column("note", TEXT)],
                        rows: &[
                            &[T("[1,2)"), T("[2020-01-01,2020-02-01)"), T("d")],
                            &[T("[1,2)"), T("[2020-03-01,2020-04-01)"), T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wd ORDER BY valid_at, note;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("valid_at", DATERANGE), Column("note", TEXT)],
                        rows: &[
                            &[T("[1,2)"), T("[2020-01-01,2020-02-01)"), T("d")],
                            &[T("[1,2)"), T("[2020-03-01,2020-04-01)"), T("a")],
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
                    query: "INSERT INTO wd VALUES ('[1,2)', '[2020-03-01,2020-04-01)', 'e');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS wd_pkey IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wd_pkey\"", detail: "Key (id, valid_at)=([1,2), [2020-03-01,2020-04-01)) conflicts with existing key (id, valid_at)=([1,2), [2020-03-01,2020-04-01)).", schema: "public", table: "wd", constraint: "wd_pkey", ..E }),
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
                    query: "INSERT INTO wd VALUES ('[1,2)', '[2020-03-15,2020-04-15)', 'f');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS wd_pkey IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wd_pkey\"", detail: "Key (id, valid_at)=([1,2), [2020-03-15,2020-04-15)) conflicts with existing key (id, valid_at)=([1,2), [2020-03-01,2020-04-01)).", schema: "public", table: "wd", constraint: "wd_pkey", ..E }),
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
                    query: "INSERT INTO wd VALUES ('[1,2)', '[2020-03-15,2020-04-15)', 'g');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM wd WHERE note = 'a';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS wd_pkey IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wd ORDER BY valid_at, note;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("valid_at", DATERANGE), Column("note", TEXT)],
                        rows: &[
                            &[T("[1,2)"), T("[2020-01-01,2020-02-01)"), T("d")],
                            &[T("[1,2)"), T("[2020-03-15,2020-04-15)"), T("g")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE wdn (id NUMRANGE, valid_at DATERANGE, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS) DEFERRABLE INITIALLY DEFERRED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wdn VALUES ('[1.0,2)', '[2020-01-01,2020-02-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wdn VALUES ('[1.00,2)', '[2020-01-01,2020-02-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wdn ORDER BY id::text;",
                    expected: Expected::Rows {
                        columns: &[Column("id", NUMRANGE), Column("valid_at", DATERANGE)],
                        rows: &[
                            &[T("[1.0,2)"), T("[2020-01-01,2020-02-01)")],
                            &[T("[1.00,2)"), T("[2020-01-01,2020-02-01)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE wdn SET valid_at = '[2020-05-01,2020-06-01)' WHERE id::text = '[1.00,2)';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wdn ORDER BY id::text;",
                    expected: Expected::Rows {
                        columns: &[Column("id", NUMRANGE), Column("valid_at", DATERANGE)],
                        rows: &[
                            &[T("[1.0,2)"), T("[2020-01-01,2020-02-01)")],
                            &[T("[1.00,2)"), T("[2020-05-01,2020-06-01)")],
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
                    query: "INSERT INTO wdn VALUES ('[1.000,2)', '[2020-01-01,2020-02-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wdn_pkey\"", detail: "Key (id, valid_at)=([1.000,2), [2020-01-01,2020-02-01)) conflicts with existing key (id, valid_at)=([1.0,2), [2020-01-01,2020-02-01)).", schema: "public", table: "wdn", constraint: "wdn_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE wd, wdn;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an insert whose period overlaps a running transaction's waits for it to end",
            set_up_script: &[
                "CREATE TABLE wc (id INT4RANGE, valid_at DATERANGE, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO wc VALUES ('[1,2)', '[2020-01-01,2020-02-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO wc VALUES ('[1,2)', '[2020-01-15,2020-02-15)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wc_pkey\"", detail: "Key (id, valid_at)=([1,2), [2020-01-15,2020-02-15)) conflicts with existing key (id, valid_at)=([1,2), [2020-01-01,2020-02-01)).", schema: "public", table: "wc", constraint: "wc_pkey", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT locktype, mode, granted FROM pg_locks WHERE NOT granted;",
                    expected: Expected::Rows {
                        columns: &[Column("locktype", TEXT), Column("mode", TEXT), Column("granted", BOOL)],
                        rows: &[
                            &[T("transactionid"), T("ShareLock"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO wc VALUES ('[2,3)', '[2020-01-01,2020-02-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO wc VALUES ('[2,3)', '[2020-02-01,2020-03-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO wc VALUES ('[3,4)', '[2020-01-01,2020-02-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO wc VALUES ('[2,3)', '[2020-01-20,2020-01-25)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM wc ORDER BY id, valid_at;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("valid_at", DATERANGE)],
                        rows: &[
                            &[T("[1,2)"), T("[2020-01-01,2020-02-01)")],
                            &[T("[2,3)"), T("[2020-01-20,2020-01-25)")],
                            &[T("[2,3)"), T("[2020-02-01,2020-03-01)")],
                            &[T("[3,4)"), T("[2020-01-01,2020-02-01)")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PERIOD foreign keys check that the referenced periods cover the referencing one, and refuse the actions Postgres refuses",
            set_up_script: &[
                "CREATE TABLE pr (id INT4RANGE, valid_at DATERANGE, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));",
                "CREATE TABLE fr (id INT4RANGE, parent_id INT4RANGE, valid_at DATERANGE, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS), FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, PERIOD valid_at));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT conname, contype, conperiod, confupdtype, confdeltype, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'fr'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("contype", CHAR), Column("conperiod", BOOL), Column("confupdtype", CHAR), Column("confdeltype", CHAR), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fr_parent_id_valid_at_fkey"), T("f"), T("t"), T("a"), T("a"), T("FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr(id, PERIOD valid_at)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pr VALUES ('[1,2)', '[2020-01-01,2020-02-01)'), ('[1,2)', '[2020-02-01,2020-03-01)'), ('[2,3)', '[2020-01-01,2020-01-10)');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fr VALUES ('[10,11)', '[1,2)', '[2020-01-15,2020-02-15)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fr VALUES ('[11,12)', '[1,2)', '[2020-02-15,2020-03-15)');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"fr\" violates foreign key constraint \"fr_parent_id_valid_at_fkey\"", detail: "Key (parent_id, valid_at)=([1,2), [2020-02-15,2020-03-15)) is not present in table \"pr\".", schema: "public", table: "fr", constraint: "fr_parent_id_valid_at_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fr VALUES ('[12,13)', '[2,3)', '[2020-01-01,2020-01-20)');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"fr\" violates foreign key constraint \"fr_parent_id_valid_at_fkey\"", detail: "Key (parent_id, valid_at)=([2,3), [2020-01-01,2020-01-20)) is not present in table \"pr\".", schema: "public", table: "fr", constraint: "fr_parent_id_valid_at_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fr VALUES ('[13,14)', '[3,4)', '[2020-01-01,2020-01-02)');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"fr\" violates foreign key constraint \"fr_parent_id_valid_at_fkey\"", detail: "Key (parent_id, valid_at)=([3,4), [2020-01-01,2020-01-02)) is not present in table \"pr\".", schema: "public", table: "fr", constraint: "fr_parent_id_valid_at_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fr VALUES ('[14,15)', NULL, '[2020-01-01,2020-01-02)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE fr SET valid_at = '[2020-01-01,2020-03-01)' WHERE id = '[10,11)';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE fr SET valid_at = '[2020-01-01,2020-03-02)' WHERE id = '[10,11)';",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"fr\" violates foreign key constraint \"fr_parent_id_valid_at_fkey\"", detail: "Key (parent_id, valid_at)=([1,2), [2020-01-01,2020-03-02)) is not present in table \"pr\".", schema: "public", table: "fr", constraint: "fr_parent_id_valid_at_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pr WHERE id = '[1,2)' AND valid_at = '[2020-02-01,2020-03-01)';",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "update or delete on table \"pr\" violates foreign key constraint \"fr_parent_id_valid_at_fkey\" on table \"fr\"", detail: "Key (id, valid_at)=([1,2), [2020-02-01,2020-03-01)) is still referenced from table \"fr\".", schema: "public", table: "fr", constraint: "fr_parent_id_valid_at_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pr SET valid_at = '[2020-02-01,2020-02-20)' WHERE id = '[1,2)' AND valid_at = '[2020-02-01,2020-03-01)';",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "update or delete on table \"pr\" violates foreign key constraint \"fr_parent_id_valid_at_fkey\" on table \"fr\"", detail: "Key (id, valid_at)=([1,2), [2020-02-01,2020-03-01)) is still referenced from table \"fr\".", schema: "public", table: "fr", constraint: "fr_parent_id_valid_at_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pr SET valid_at = '[2019-12-01,2020-02-01)' WHERE id = '[1,2)' AND valid_at = '[2020-01-01,2020-02-01)';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pr WHERE id = '[2,3)';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pr VALUES ('[2,3)', '[2020-01-01,2020-01-10)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fr ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("parent_id", INT4RANGE), Column("valid_at", DATERANGE)],
                        rows: &[
                            &[T("[10,11)"), T("[1,2)"), T("[2020-01-01,2020-03-01)")],
                            &[T("[14,15)"), Null, T("[2020-01-01,2020-01-02)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr DROP CONSTRAINT fr_parent_id_valid_at_fkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fr VALUES ('[15,16)', '[2,3)', '[2020-01-05,2020-01-20)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, PERIOD valid_at);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"fr\" violates foreign key constraint \"fr_fk\"", detail: "Key (parent_id, valid_at)=([2,3), [2020-01-05,2020-01-20)) is not present in table \"pr\".", schema: "public", table: "fr", constraint: "fr_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM fr WHERE id = '[15,16)';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, PERIOD valid_at);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, PERIOD valid_at) ON DELETE CASCADE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported ON DELETE action for foreign key constraint using PERIOD", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, PERIOD valid_at) ON UPDATE SET NULL;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported ON UPDATE action for foreign key constraint using PERIOD", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, PERIOD valid_at) ON DELETE SET DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported ON DELETE action for foreign key constraint using PERIOD", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, PERIOD valid_at) ON DELETE RESTRICT;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported ON DELETE action for foreign key constraint using PERIOD", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, PERIOD valid_at) ON UPDATE CASCADE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported ON UPDATE action for foreign key constraint using PERIOD", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, valid_at) REFERENCES pr (id, PERIOD valid_at);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "foreign key uses PERIOD on the referenced table but not the referencing table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr (id, valid_at);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "foreign key uses PERIOD on the referencing table but not the referenced table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, PERIOD id) REFERENCES pr (id, PERIOD valid_at);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "foreign key constraint \"fr_fk2\" cannot be implemented", detail: "Key columns \"id\" of the referencing table and \"valid_at\" of the referenced table are of incompatible types: int4range and daterange.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk2 FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'fr'::regclass AND contype = 'f' ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fr_fk"), T("FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr(id, PERIOD valid_at)")],
                            &[T("fr_fk2"), T("FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pr(id, PERIOD valid_at)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pu (id INT4RANGE, valid_at DATERANGE, UNIQUE (id, valid_at));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fr ADD CONSTRAINT fr_fk3 FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pu (id, PERIOD valid_at);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "there is no unique constraint matching given keys for referenced table \"pu\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pm (id INT4RANGE, valid_at DATEMULTIRANGE, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fm (id INT4RANGE, parent_id INT4RANGE, valid_at DATEMULTIRANGE, FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES pm (id, PERIOD valid_at) DEFERRABLE INITIALLY DEFERRED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pm VALUES ('[1,2)', '{[2020-01-01,2020-02-01)}'), ('[1,2)', '{[2020-02-01,2020-03-01)}');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fm VALUES ('[10,11)', '[1,2)', '{[2020-01-15,2020-02-15), [2020-04-01,2020-04-02)}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pm VALUES ('[1,2)', '{[2020-04-01,2020-05-01)}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fm VALUES ('[11,12)', '[1,2)', '{[2020-06-01,2020-06-02)}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"fm\" violates foreign key constraint \"fm_parent_id_valid_at_fkey\"", detail: "Key (parent_id, valid_at)=([1,2), {[2020-06-01,2020-06-02)}) is not present in table \"pm\".", schema: "public", table: "fm", constraint: "fm_parent_id_valid_at_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fm ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("parent_id", INT4RANGE), Column("valid_at", DATEMULTIRANGE)],
                        rows: &[
                            &[T("[10,11)"), T("[1,2)"), T("{[2020-01-15,2020-02-15),[2020-04-01,2020-04-02)}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pm WHERE valid_at = '{[2020-04-01,2020-05-01)}';",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "update or delete on table \"pm\" violates foreign key constraint \"fm_parent_id_valid_at_fkey\" on table \"fm\"", detail: "Key (id, valid_at)=([1,2), {[2020-04-01,2020-05-01)}) is still referenced from table \"fm\".", schema: "public", table: "fm", constraint: "fm_parent_id_valid_at_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE fm, pm, pu, fr, pr;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "REINDEX writes a WITHOUT OVERLAPS key's GiST tree again, inside a transaction too",
            set_up_script: &[
                "CREATE TABLE rp (id INT4RANGE, valid_at DATERANGE, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));",
                "CREATE TABLE ru (k INT PRIMARY KEY, id INT4RANGE, valid_at DATERANGE, UNIQUE (id, valid_at WITHOUT OVERLAPS));",
                "INSERT INTO rp SELECT int4range(g, g + 1), daterange('2020-01-01', '2020-02-01') FROM generate_series(1, 20) g;",
                "INSERT INTO ru SELECT g, int4range(g, g + 1), daterange('2020-01-01', '2020-02-01') FROM generate_series(1, 20) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "REINDEX INDEX rp_pkey;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX ru_id_valid_at_key;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE rp;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rp VALUES ('[1,2)', '[2020-03-01,2020-04-01)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX rp_pkey;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lower(id), valid_at FROM rp WHERE id && '[1,3)' ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("lower", INT4), Column("valid_at", DATERANGE)],
                        rows: &[
                            &[T("1"), T("[2020-01-01,2020-02-01)")],
                            &[T("1"), T("[2020-03-01,2020-04-01)")],
                            &[T("2"), T("[2020-01-01,2020-02-01)")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lower(id), valid_at FROM ru WHERE id && '[1,3)' ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("lower", INT4), Column("valid_at", DATERANGE)],
                        rows: &[
                            &[T("1"), T("[2020-01-01,2020-02-01)")],
                            &[T("2"), T("[2020-01-01,2020-02-01)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rp VALUES ('[2,3)', '[2020-01-15,2020-01-16)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"rp_pkey\"", detail: "Key (id, valid_at)=([2,3), [2020-01-15,2020-01-16)) conflicts with existing key (id, valid_at)=([2,3), [2020-01-01,2020-02-01)).", schema: "public", table: "rp", constraint: "rp_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ru VALUES (21, '[2,3)', '[2020-01-15,2020-01-16)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"ru_id_valid_at_key\"", detail: "Key (id, valid_at)=([2,3), [2020-01-15,2020-01-16)) conflicts with existing key (id, valid_at)=([2,3), [2020-01-01,2020-02-01)).", schema: "public", table: "ru", constraint: "ru_id_valid_at_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltuples FROM pg_class WHERE relname IN ('rp', 'rp_pkey', 'ru', 'ru_id_valid_at_key') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("rp"), T("21")],
                            &[T("rp_pkey"), T("21")],
                            &[T("ru"), T("20")],
                            &[T("ru_id_valid_at_key"), T("20")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rp, ru;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_gist_exclusion_checks_scale_with_the_transactions_own_entries() {
    run_scripts(&[
        ScriptTest {
            name: "a WITHOUT OVERLAPS key's build and an exclusion constraint's bulk inserts over 10,000 rows finish within a statement timeout",
            set_up_script: &[
                "CREATE TABLE sp (id INT4RANGE, valid_at DATERANGE);",
                "INSERT INTO sp SELECT int4range(g, g + 1), daterange('2020-01-01', '2020-02-01') FROM generate_series(1, 10000) g;",
                "CREATE TABLE se (r INT4RANGE, EXCLUDE USING gist (r WITH &&));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET statement_timeout = '30s';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sp ADD PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sp VALUES ('[5000,5001)', '[2020-01-15,2020-01-16)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"sp_pkey\"", detail: "Key (id, valid_at)=([5000,5001), [2020-01-15,2020-01-16)) conflicts with existing key (id, valid_at)=([5000,5001), [2020-01-01,2020-02-01)).", schema: "public", table: "sp", constraint: "sp_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO se SELECT int4range(g * 2, g * 2 + 1) FROM generate_series(1, 10000) g;",
                    expected: Expected::Tag("INSERT 0 10000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO se SELECT int4range(g * 2 + 1, g * 2 + 2) FROM generate_series(1, 10000) g;",
                    expected: Expected::Tag("INSERT 0 10000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM se WHERE r && '[100,110)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO se VALUES ('[9001,9002)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"se_r_excl\"", detail: "Key (r)=([9001,9002)) conflicts with existing key (r)=([9001,9002)).", schema: "public", table: "se", constraint: "se_r_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET statement_timeout;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE sp, se;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a GiST index made again under its name with another operator class, or as a btree, finds its rows before and after its transaction ends",
            set_up_script: &[
                "CREATE TABLE rk (id INT PRIMARY KEY, r INT4RANGE, p POINT);",
                "INSERT INTO rk SELECT g, int4range(g, g + 3), point(g, g) FROM generate_series(1, 2000) g;",
                "CREATE INDEX rk_i ON rk USING gist (r);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rk WHERE r && '[100,105)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX rk_i;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX rk_i ON rk USING gist (p);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rk SELECT g, int4range(g, g + 3), point(g, g) FROM generate_series(2001, 3000) g;",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rk WHERE p <@ box '(100,100),(110,110)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rk WHERE p <@ box '(2500,2500),(2510,2510)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rk WHERE p <@ box '(100,100),(110,110)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rk WHERE p <@ box '(2500,2500),(2510,2510)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX rk_i;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX rk_i ON rk USING gist (r);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rk SELECT g, int4range(g, g + 3), point(g, g) FROM generate_series(3001, 4000) g;",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rk WHERE r && '[3500,3502)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
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
                    query: "SELECT count(*) FROM rk WHERE p <@ box '(2500,2500),(2510,2510)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX rk_i;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX rk_i ON rk (id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rk WHERE id BETWEEN 100 AND 110;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX rk_j ON rk USING gist (r);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rk WHERE r && '[100,105)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE rk;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_gist_ranges_over_a_subtype_its_comparison_function_orders() {
    run_scripts(&[
        ScriptTest {
            name: "range_ops and multirange_ops over a subtype ordered by a SQL comparison function, whose errors fail the statement",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT4 LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = 13 THEN 1 / ($1 - 13) WHEN $2 = 13 THEN 1 / ($2 - 13) ELSE btint4cmp($2, $1) END $$;",
                "CREATE OPERATOR CLASS int4_rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 > (INT4, INT4), OPERATOR 2 >= (INT4, INT4), OPERATOR 3 = (INT4, INT4), OPERATOR 4 <= (INT4, INT4), OPERATOR 5 < (INT4, INT4), FUNCTION 1 rev_cmp(INT4, INT4);",
                "CREATE TYPE revrange AS RANGE (SUBTYPE = INT4, SUBTYPE_OPCLASS = int4_rev_ops);",
                "CREATE TABLE rr (id INT PRIMARY KEY, v revrange);",
                "INSERT INTO rr SELECT g, revrange(g * 3 + 20, g * 3) FROM generate_series(100, 2100) g;",
                "CREATE INDEX rr_v ON rr USING gist (v);",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM rr WHERE v && revrange(1020, 1000) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: id")],
                            &[T("  ->  Bitmap Heap Scan on rr")],
                            &[T("        Recheck Cond: (v && '[1020,1000)'::revrange)")],
                            &[T("        ->  Bitmap Index Scan on rr_v")],
                            &[T("              Index Cond: (v && '[1020,1000)'::revrange)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rr WHERE v && revrange(1020, 1000) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("327")],
                            &[T("328")],
                            &[T("329")],
                            &[T("330")],
                            &[T("331")],
                            &[T("332")],
                            &[T("333")],
                            &[T("334")],
                            &[T("335")],
                            &[T("336")],
                            &[T("337")],
                            &[T("338")],
                            &[T("339")],
                        ],
                        tag: "SELECT 13",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rr WHERE v @> 4000;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rr VALUES (5001, revrange(100000, 90000));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rr VALUES (5000, revrange(13, NULL));",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM rr WHERE id BETWEEN 300 AND 400;",
                    expected: Expected::Tag("DELETE 101"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rr SET v = revrange(5, 1) WHERE id = 500;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rr WHERE v && revrange(1020, 1000) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rr WHERE v && revrange(95000, 94000) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rr WHERE v <@ revrange(10, 0) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM rr WHERE v && revrange(1300, 900);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("39")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rm (id INT PRIMARY KEY, v revmultirange);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rm SELECT g, revmultirange(revrange(g * 3 + 20, g * 3), revrange(g * 3 + 2000, g * 3 + 1990)) FROM generate_series(100, 600) g;",
                    expected: Expected::Tag("INSERT 0 501"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX rm_v ON rm USING gist (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rm WHERE v && revrange(1020, 1000) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("327")],
                            &[T("328")],
                            &[T("329")],
                            &[T("330")],
                            &[T("331")],
                            &[T("332")],
                            &[T("333")],
                            &[T("334")],
                            &[T("335")],
                            &[T("336")],
                            &[T("337")],
                            &[T("338")],
                            &[T("339")],
                        ],
                        tag: "SELECT 13",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
