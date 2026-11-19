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
fn test_extended_statistics_data() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE computes extended statistics' data byte for byte as Postgres does",
            set_up_script: &[
                "CREATE TABLE es (a INT, b INT, c TEXT, d NUMERIC);",
                "INSERT INTO es VALUES (1, 1, 'x', 1.5), (2, 2, 'y', 2.5), (3, 3, 'z', 3.5), (4, 4, 'x', 4.5), (5, 0, 'y', 5.5), (6, 1, 'z', 6.5), (7, 2, 'x', 7.5), (8, 3, NULL, 8.5);",
                "INSERT INTO es SELECT a + 8, (a + 8) % 5, c, d FROM es;",
                "INSERT INTO es SELECT a + 16, (a + 16) % 5, c, d FROM es;",
                "INSERT INTO es SELECT a + 32, (a + 32) % 5, c, d FROM es;",
                "INSERT INTO es SELECT a + 64, (a + 64) % 5, c, d FROM es;",
                "INSERT INTO es SELECT a + 128, (a + 128) % 5, c, d FROM es;",
                "UPDATE es SET a = a % 10;",
                "CREATE STATISTICS es_nd (ndistinct) ON a, b, c FROM es;",
                "CREATE STATISTICS es_dep (dependencies) ON a, b FROM es;",
                "CREATE STATISTICS es_mcv (mcv) ON a, b, c FROM es;",
                "CREATE STATISTICS es_all ON b, c, d FROM es;",
                "CREATE STATISTICS es_expr ON (a + b), (lower(c)) FROM es;",
                "CREATE STATISTICS es_mixed (ndistinct, mcv) ON a, (b * 2) FROM es;",
                "ANALYZE es;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT s.stxname, d.stxdinherit, d.stxdndistinct, d.stxddependencies FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid ORDER BY s.stxname;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME), Column("stxdinherit", BOOL), Column("stxdndistinct", PG_NDISTINCT), Column("stxddependencies", PG_DEPENDENCIES)],
                        rows: &[
                            &[T("es_all"), T("f"), T(r#"{"2, 3": 20, "2, 4": 40, "3, 4": 8, "2, 3, 4": 40}"#), T(r#"{"3 => 4": 0.125000, "4 => 3": 1.000000, "2, 3 => 4": 0.125000, "2, 4 => 3": 1.000000}"#)],
                            &[T("es_dep"), T("f"), Null, T(r#"{"1 => 2": 1.000000}"#)],
                            &[T("es_expr"), T("f"), T(r#"{"-1, -2": 35}"#), Null],
                            &[T("es_mcv"), T("f"), Null, Null],
                            &[T("es_mixed"), T("f"), T(r#"{"1, -1": 10}"#), Null],
                            &[T("es_nd"), T("f"), T(r#"{"1, 2": 10, "1, 3": 35, "2, 3": 20, "1, 2, 3": 35}"#), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxname, d.stxdmcv FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid ORDER BY s.stxname;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME), Column("stxdmcv", PG_MCV_LIST)],
                        rows: &[
                            &[T("es_all"), T(r#"\xc251a6e1010000002800000003001700000019000000a40600000500000014000000000000000400000001000000030000000f00000018000000ffffffff00000000080000005000000080000000ffffffff00000000000000000100000002000000030000000400000001000000780100000079010000007a06000000808001008813060000008080020088130600000080800300881306000000808004008813060000008080050088130600000080800600881306000000808007008813060000008080080088130000000000000000009c3f0000000000007a3f0100020005000001000000000000009c3f0000000000006a3f0100000007000000000000000000009c3f000000000020833f0000000006000000000000000000009c3f000000000020833f0200000003000000000000000000009c3f000000000080793f0300010004000000000000000000009c3f000000000080793f0300020002000000000000000000009c3f000000000020833f0200000006000001000000000000009c3f000000000080693f0300000007000000000000000000009c3f000000000020833f0400000000000000000000000000009c3f000000000020833f0400000003000000000000000000009c3f000000000080793f0200010001000000000000000000009c3f000000000080793f0000010001000000000000000000009c3f000000000080793f0000010004000000000000000000009c3f000000000080833f0100000000000000000000000000009c3f000000000080793f0400020005000000000000000000009c3f0000000000007a3f010002000200000000000000000000983f000000000020833f030000000600000000000000000000983f000000000080793f030001000100000000000000000000983f000000000080793f030002000500000000000000000000983f000000000020833f040000000600000000000000000000983f000000000080793f040001000100000000000000000000983f000000000080793f040001000400000000000000000000983f000000000080793f040002000200000000000000000000983f000000000020833f000000000000000100000000000000983f000000000080693f040000000700000000000000000000983f000000000020833f000000000300000000000000000000983f000000000080793f000002000200000000000000000000983f000000000080793f000002000500000100000000000000983f000000000080693f000000000700000000000000000000983f000000000080833f010000000300000000000000000000983f000000000080833f010000000600000000000000000000983f0000000000007a3f010001000100000000000000000000983f0000000000007a3f010001000400000000000000000000983f000000000020833f020000000000000000000000000000983f000000000080793f020001000400000000000000000000983f000000000080793f020002000200000000000000000000983f000000000080793f020002000500000100000000000000983f000000000080693f020000000700000000000000000000983f000000000020833f030000000000000000000000000000983f000000000020833f030000000300"#)],
                            &[T("es_dep"), Null],
                            &[T("es_expr"), T(r#"\xc251a6e10100000023000000020017000000190000000a00000028000000000000000400000001000000030000000f00000018000000ffffffff0000000000000000020000000400000005000000060000000700000008000000090000000b0000000d00000001000000780100000079010000007a0000000000000000aa3f000000000080a33f030000000000000000000000aa3f000000000080a33f010000000000000000000000aa3f0000000000c0a23f090000000000000000000000aa3f0000000000c0a23f070000000000000000000000a83f000000000080a33f0400000000000000000000009c3f000000000000993f0000010000000000000000009c3f0000000000009a3f0300010000000000000000009c3f0000000000009a3f0100020000000000000000009c3f0000000000009a3f0400010000010000000000009c3f000000000000893f0800000000000000000000009c3f0000000000009a3f0400020000000000000000009c3f0000000000009a3f0200010000000000000000009c3f000000000080a33f0200000000000000000000009c3f0000000000009a3f0500020000010000000000009c3f0000000000008a3f0500000000000000000000009c3f000000000080a33f0600000000000000000000009c3f0000000000009a3f060002000000000000000000983f0000000000c0a23f000000000000000000000000983f000000000000993f090002000000000000000000983f000000000000993f000002000001000000000000983f000000000000893f000000000000000000000000983f0000000000009a3f010001000000000000000000983f0000000000009a3f020002000001000000000000983f0000000000008a3f020000000000000000000000983f0000000000009a3f030002000000000000000000983f000000000080a33f050000000000000000000000983f0000000000009a3f050001000000000000000000983f0000000000009a3f060001000001000000000000983f0000000000008a3f060000000000000000000000983f000000000000993f070001000000000000000000983f000000000000993f070002000000000000000000983f0000000000c0a23f080000000000000000000000983f000000000000993f080001000000000000000000983f000000000000993f080002000000000000000000983f000000000000993f09000100"#)],
                            &[T("es_mcv"), T(r#"\xc251a6e1010000002300000003001700000017000000190000000a000000280000000000000004000000010000000500000014000000000000000400000001000000030000000f00000018000000ffffffff0000000000000000010000000200000003000000040000000500000006000000070000000800000009000000000000000100000002000000030000000400000001000000780100000079010000007a000000000000000000aa3f0000000000e27d3f070002000000000000000000000000aa3f0000000000e27d3f090004000000000000000000000000aa3f0000000000147f3f050000000000000000000000000000aa3f0000000000b07f3f010001000000000000000000000000a83f0000000000147f3f0300030000000000000000000000009c3f000000000020753f0600010002000000010000000000009c3f000000000020653f0600010000000000000000000000009c3f0000000000ec733f0000000001000000000000000000009c3f0000000000b8743f0200020001000000000000000000009c3f0000000000b8743f0300030001000000000000000000009c3f0000000000b8743f0300030002000000000000000000009c3f0000000000147f3f0400040000000000000000000000009c3f000000000020753f0100010002000000010000000000009c3f0000000000ec633f0800030000000000000000000000009c3f0000000000b8743f0500000001000000000000000000009c3f0000000000b8743f0400040002000000000000000000009c3f0000000000147f3f020002000000000000000000000000983f0000000000e27d3f000000000000000000000000000000983f0000000000ec733f090004000200000000000000000000983f0000000000ec733f000000000200000001000000000000983f0000000000ec633f000000000000000000000000000000983f000000000020753f010001000100000000000000000000983f0000000000b8743f020002000200000001000000000000983f0000000000b8643f020002000000000000000000000000983f0000000000b8743f040004000100000001000000000000983f0000000000b8643f040004000000000000000000000000983f0000000000b8743f050000000200000000000000000000983f0000000000b07f3f060001000000000000000000000000983f000000000020753f060001000100000000000000000000983f0000000000ec733f070002000100000000000000000000983f0000000000ec733f070002000200000000000000000000983f0000000000e27d3f080003000000000000000000000000983f0000000000ec733f080003000100000000000000000000983f0000000000ec733f080003000200000000000000000000983f0000000000ec733f090004000100"#)],
                            &[T("es_mixed"), T(r#"\xc251a6e1010000000a000000020017000000170000000a0000002800000000000000040000000100000005000000140000000000000004000000010000000000000001000000020000000300000004000000050000000600000007000000080000000900000000000000020000000400000006000000080000000000000000000000ba3f000000000020953f060001000000000000000000ba3f000000000020953f010001000000000000000000ba3f0000000000b8943f020002000000000000000000ba3f0000000000b8943f030003000000000000000000ba3f0000000000b8943f040004000000000000000000ba3f0000000000b8943f050000000000000000000000b93f0000000000ec933f090004000000000000000000b93f0000000000ec933f070002000000000000000000b93f0000000000ec933f080003000000000000000000b93f0000000000ec933f00000000"#)],
                            &[T("es_nd"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxname, d.stxdexpr IS NULL AS no_expr_stats FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid ORDER BY s.stxname;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME), Column("no_expr_stats", BOOL)],
                        rows: &[
                            &[T("es_all"), T("t")],
                            &[T("es_dep"), T("t")],
                            &[T("es_expr"), T("f")],
                            &[T("es_mcv"), T("t")],
                            &[T("es_mixed"), T("f")],
                            &[T("es_nd"), T("t")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS es_nd;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_statistic_ext_data;",
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
                    query: "ALTER STATISTICS es_dep SET STATISTICS 0;",
                    expected: Expected::Tag("ALTER STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE es;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxname, d.stxddependencies FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid WHERE s.stxname = 'es_dep';",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME), Column("stxddependencies", PG_DEPENDENCIES)],
                        rows: &[
                            &[T("es_dep"), T(r#"{"1 => 2": 1.000000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE es (a);",
                    expected: Expected::Tag("ANALYZE"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42P17", message: r#"statistics object "public.es_dep" could not be computed for relation "public.es""#, schema: "public", table: "es", ..N }, Diagnostic { severity: "WARNING", code: "42P17", message: r#"statistics object "public.es_mcv" could not be computed for relation "public.es""#, schema: "public", table: "es", ..N }, Diagnostic { severity: "WARNING", code: "42P17", message: r#"statistics object "public.es_all" could not be computed for relation "public.es""#, schema: "public", table: "es", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_ndistinct_in('x');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot accept a value of type pg_ndistinct", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ep (a INT, b INT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ep1 PARTITION OF ep FOR VALUES FROM (0) TO (100);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ep2 PARTITION OF ep FOR VALUES FROM (100) TO (1000);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ep SELECT a, b FROM es;",
                    expected: Expected::Tag("INSERT 0 256"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ep SELECT a + 100, b FROM es;",
                    expected: Expected::Tag("INSERT 0 256"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ep_s (ndistinct, dependencies) ON a, b FROM ep;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ep;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxname, d.stxdinherit, d.stxdndistinct, d.stxddependencies FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid WHERE s.stxname = 'ep_s' ORDER BY d.stxdinherit;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME), Column("stxdinherit", BOOL), Column("stxdndistinct", PG_NDISTINCT), Column("stxddependencies", PG_DEPENDENCIES)],
                        rows: &[
                            &[T("ep_s"), T("t"), T(r#"{"1, 2": 20}"#), T(r#"{"1 => 2": 1.000000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ei (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE eic () INHERITS (ei);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ei VALUES (1, 1), (2, 2), (1, 1);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO eic VALUES (3, 3), (3, 3), (4, 4);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ei_s (ndistinct, mcv) ON a, b FROM ei;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ei;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxname, d.stxdinherit, d.stxdndistinct, d.stxdmcv FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid WHERE s.stxname = 'ei_s' ORDER BY d.stxdinherit;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME), Column("stxdinherit", BOOL), Column("stxdndistinct", PG_NDISTINCT), Column("stxdmcv", PG_MCV_LIST)],
                        rows: &[
                            &[T("ei_s"), T("f"), T(r#"{"1, 2": 2}"#), T(r#"\xc251a6e101000000020000000200170000001700000002000000080000000000000004000000010000000200000008000000000000000400000001000000010000000200000001000000020000000000555555555555e53f1cc7711cc771dc3f000000000000555555555555d53f1cc7711cc771bc3f01000100"#)],
                            &[T("ei_s"), T("t"), T(r#"{"1, 2": 4}"#), T(r#"\xc251a6e10100000004000000020017000000170000000400000010000000000000000400000001000000040000001000000000000000040000000100000001000000020000000300000004000000010000000200000003000000040000000000555555555555d53f1cc7711cc771bc3f000000000000555555555555d53f1cc7711cc771bc3f020002000000555555555555c53f1cc7711cc7719c3f010001000000555555555555c53f1cc7711cc7719c3f03000300"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ei VALUES (5, 6), (7, 8), (9, 10), (11, 12);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.stxdinherit, d.stxdndistinct FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid WHERE s.stxname = 'ei_s' ORDER BY d.stxdinherit;",
                    expected: Expected::Rows {
                        columns: &[Column("stxdinherit", BOOL), Column("stxdndistinct", PG_NDISTINCT)],
                        rows: &[
                            &[T("f"), T(r#"{"1, 2": 2}"#)],
                            &[T("t"), T(r#"{"1, 2": 4}"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ei;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.stxdinherit, d.stxdndistinct FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid WHERE s.stxname = 'ei_s' ORDER BY d.stxdinherit;",
                    expected: Expected::Rows {
                        columns: &[Column("stxdinherit", BOOL), Column("stxdndistinct", PG_NDISTINCT)],
                        rows: &[
                            &[T("f"), T(r#"{"1, 2": 6}"#)],
                            &[T("t"), T(r#"{"1, 2": 8}"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "branches with equal table contents get equal extended statistics' data",
            set_up_script: &[
                "CREATE TABLE eb (a INT, b INT);",
                "INSERT INTO eb VALUES (1, 1), (1, 1), (2, 2), (3, 3);",
                "CREATE STATISTICS eb_s (ndistinct, dependencies) ON a, b FROM eb;",
                "SELECT DOLT_COMMIT('-Am', 'base');",
                "SELECT DOLT_BRANCH('other');",
                "ANALYZE eb;",
                "SELECT DOLT_CHECKOUT('other');",
                "ANALYZE eb;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT d.stxdndistinct, d.stxddependencies FROM pg_statistic_ext_data d;",
                    expected: Expected::Rows {
                        columns: &[Column("stxdndistinct", PG_NDISTINCT), Column("stxddependencies", PG_DEPENDENCIES)],
                        rows: &[
                            &[T(r#"{"1, 2": 3}"#), T(r#"{"1 => 2": 1.000000, "2 => 1": 1.000000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", TEXT_ARRAY)],
                        rows: &[
                            &[T(r#"{0,"Switched to branch 'main'"}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.stxdndistinct, d.stxddependencies FROM pg_statistic_ext_data d;",
                    expected: Expected::Rows {
                        columns: &[Column("stxdndistinct", PG_NDISTINCT), Column("stxddependencies", PG_DEPENDENCIES)],
                        rows: &[
                            &[T(r#"{"1, 2": 3}"#), T(r#"{"1 => 2": 1.000000, "2 => 1": 1.000000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_checkout returns a record, where the expectation is a text array"),
            ..S
        },
    ]);
}

#[test]
fn test_extended_statistics_estimates() {
    run_scripts(&[
        ScriptTest {
            name: "extended statistics' estimates as Postgres makes them, from stats_ext.sql",
            set_up_script: &[
                r#"create function check_estimated_rows(text) returns text language plpgsql as $$ declare ln text; tmp text[]; first_row bool := true; begin for ln in execute format('explain analyze %s', $1) loop if first_row then first_row := false; tmp := regexp_match(ln, 'rows=(\d*) .* rows=(\d*)'); return tmp[1] || ' ' || tmp[2]; end if; end loop; end; $$;"#,
                "CREATE TABLE stxdinh(a int, b int);",
                "CREATE TABLE stxdinh1() INHERITS(stxdinh);",
                "CREATE TABLE stxdinh2() INHERITS(stxdinh);",
                "INSERT INTO stxdinh SELECT mod(a,50), mod(a,100) FROM generate_series(0, 1999) a;",
                "INSERT INTO stxdinh1 SELECT mod(a,100), mod(a,100) FROM generate_series(0, 999) a;",
                "INSERT INTO stxdinh2 SELECT mod(a,100), mod(a,100) FROM generate_series(0, 999) a;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "VACUUM ANALYZE stxdinh, stxdinh1, stxdinh2;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT a, b FROM stxdinh* GROUP BY 1, 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("400 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT a, b FROM stxdinh* WHERE a = 0 AND b = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 40")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS stxdinh ON a, b FROM stxdinh;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM ANALYZE stxdinh, stxdinh1, stxdinh2;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT a, b FROM stxdinh* GROUP BY 1, 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("150 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT a, b FROM stxdinh* WHERE a = 0 AND b = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("22 40")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT a, b FROM ONLY stxdinh GROUP BY 1, 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT a, b FROM ONLY stxdinh WHERE a = 0 AND b = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("20 20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE stxdinh, stxdinh1, stxdinh2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE stxdinp(i int, a int, b int) PARTITION BY RANGE (i);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE stxdinp1 PARTITION OF stxdinp FOR VALUES FROM (1) TO (100);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO stxdinp SELECT 1, a/100, a/100 FROM generate_series(1, 999) a;",
                    expected: Expected::Tag("INSERT 0 999"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS stxdinp ON (a + 1), a, b FROM stxdinp;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM ANALYZE stxdinp;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 FROM pg_statistic_ext WHERE stxrelid = 'stxdinp'::regclass;",
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
                    query: "SELECT check_estimated_rows('SELECT a, b FROM stxdinp GROUP BY 1, 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("10 10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT a + 1, b FROM ONLY stxdinp GROUP BY 1, 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE stxdinp;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ab1 (a INTEGER, b INTEGER, c TIMESTAMP, d TIMESTAMPTZ);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ab1_exprstat_1 ON (a+b) FROM ab1;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ab1_exprstat_2 ON (a+b) FROM ab1;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stxkind FROM pg_statistic_ext WHERE stxname = 'ab1_exprstat_2';",
                    expected: Expected::Rows {
                        columns: &[Column("stxkind", CHAR_ARRAY)],
                        rows: &[
                            &[T("{e}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ab1_exprstat_3 ON (a+b), a FROM ab1;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stxkind FROM pg_statistic_ext WHERE stxname = 'ab1_exprstat_3';",
                    expected: Expected::Rows {
                        columns: &[Column("stxkind", CHAR_ARRAY)],
                        rows: &[
                            &[T("{d,f,m,e}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ab1_exprstat_4 ON date_trunc('day', d) FROM ab1;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ab1_exprstat_5 ON date_trunc('day', c) FROM ab1;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS ab1_exprstat_6 ON (case a when 1 then true else false end), b FROM ab1;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ab1 SELECT x / 10, x / 3, '2020-10-01'::timestamp + x * interval '1 day', '2020-10-01'::timestamptz + x * interval '1 day' FROM generate_series(1, 100) x;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ab1;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM ab1 WHERE (case a when 1 then true else false end) AND b=2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ab1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ndistinct ( filler1 TEXT, filler2 NUMERIC, a INT, b INT, filler3 DATE, c INT, d INT ) WITH (autovacuum_enabled = off);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ndistinct (a, b, c, filler1) SELECT i/100, i/100, i/100, (i/100) || ' dollars and zero cents' FROM generate_series(1,1000) s(i);",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY b, c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (a+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100), (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s10 ON a, b, c FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxkind, d.stxdndistinct FROM pg_statistic_ext s, pg_statistic_ext_data d WHERE s.stxrelid = 'ndistinct'::regclass AND d.stxoid = s.oid;",
                    expected: Expected::Rows {
                        columns: &[Column("stxkind", CHAR_ARRAY), Column("stxdndistinct", PG_NDISTINCT)],
                        rows: &[
                            &[T("{d,f,m}"), T(r#"{"3, 4": 11, "3, 6": 11, "4, 6": 11, "3, 4, 6": 11}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY ctid, a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1000 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("11 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY b, c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("11 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("11 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (a+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("11 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("11 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100), (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("11 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("11 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE TABLE ndistinct;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ndistinct (a, b, c, filler1) SELECT mod(i,13), mod(i,17), mod(i,19), mod(i,23) || ' dollars and zero cents' FROM generate_series(1,1000) s(i);",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxkind, d.stxdndistinct FROM pg_statistic_ext s, pg_statistic_ext_data d WHERE s.stxrelid = 'ndistinct'::regclass AND d.stxoid = s.oid;",
                    expected: Expected::Rows {
                        columns: &[Column("stxkind", CHAR_ARRAY), Column("stxdndistinct", PG_NDISTINCT)],
                        rows: &[
                            &[T("{d,f,m}"), T(r#"{"3, 4": 221, "3, 6": 247, "4, 6": 323, "3, 4, 6": 1000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("221 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1000 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1000 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("323 323")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 13")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (a+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("221 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("221 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100), (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1000 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("221 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s10;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxkind, d.stxdndistinct FROM pg_statistic_ext s, pg_statistic_ext_data d WHERE s.stxrelid = 'ndistinct'::regclass AND d.stxoid = s.oid;",
                    expected: Expected::Rows {
                        columns: &[Column("stxkind", CHAR_ARRAY), Column("stxdndistinct", PG_NDISTINCT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 323")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, d');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 13")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (a+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100), (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100), (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s10 (ndistinct) ON (a+1), (b+100), (2*c) FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxkind, d.stxdndistinct FROM pg_statistic_ext s, pg_statistic_ext_data d WHERE s.stxrelid = 'ndistinct'::regclass AND d.stxoid = s.oid;",
                    expected: Expected::Rows {
                        columns: &[Column("stxkind", CHAR_ARRAY), Column("stxdndistinct", PG_NDISTINCT)],
                        rows: &[
                            &[T("{d,e}"), T(r#"{"-1, -2": 221, "-1, -3": 247, "-2, -3": 323, "-1, -2, -3": 1000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("221 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a+1), (b+100), (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1000 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (a+1), (b+100)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("221 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s10;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 247")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s10 (ndistinct) ON a, b, (2*c) FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.stxkind, d.stxdndistinct FROM pg_statistic_ext s, pg_statistic_ext_data d WHERE s.stxrelid = 'ndistinct'::regclass AND d.stxoid = s.oid;",
                    expected: Expected::Rows {
                        columns: &[Column("stxkind", CHAR_ARRAY), Column("stxdndistinct", PG_NDISTINCT)],
                        rows: &[
                            &[T("{d,e}"), T(r#"{"3, 4": 221, "3, -1": 247, "4, -1": 323, "3, 4, -1": 1000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("221 221")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("247 247")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (2*c)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1000 1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s10;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE ndistinct;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ndistinct (a, b, c, d) SELECT mod(i,3), mod(i,9), mod(i,5), mod(i,20) FROM generate_series(1,1000) s(i);",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("27 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("27 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("27 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("27 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1), c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (c*10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1), c, (d - 1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 180")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s11 (ndistinct) ON a, b FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s12 (ndistinct) ON c, d FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1), c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (c*10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1), c, (d - 1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 180")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s12;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s12 (ndistinct) ON (c * 10), (d - 1) FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1), c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (c*10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1), c, (d - 1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 180")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s12;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s12 (ndistinct) ON c, d, (c * 10), (d - 1) FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1), c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (c*10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1), c, (d - 1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 180")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s11;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s11 (ndistinct) ON a, b, (a*5), (b+1) FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1), c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (c*10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1), c, (d - 1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 180")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s11;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s12;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s11 (ndistinct) ON a, b, (a*5), (b+1) FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS s12 (ndistinct) ON a, (b+1), (c * 10) FROM ndistinct;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ndistinct;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("9 9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY (a*5), (b+1), c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("45 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, b, (c*10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 45")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT COUNT(*) FROM ndistinct GROUP BY a, (b+1), c, (d - 1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 180")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s11;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS s12;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE functional_dependencies ( filler1 TEXT, filler2 NUMERIC, a INT, b TEXT, filler3 DATE, c INT, d TEXT ) WITH (autovacuum_enabled = off);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX fdeps_ab_idx ON functional_dependencies (a, b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX fdeps_abc_idx ON functional_dependencies (a, b, c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO functional_dependencies (a, b, c, filler1) SELECT mod(i, 5), mod(i, 7), mod(i, 11), i FROM generate_series(1,1000) s(i);",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("29 29")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS func_deps_stat (dependencies) ON a, b, c FROM functional_dependencies;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("29 29")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE functional_dependencies;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS func_deps_stat;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO functional_dependencies (a, b, c, filler1) SELECT i, i, i, i FROM generate_series(1,5000) s(i);",
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE mod(a, 11) = 1 AND mod(b::int, 13) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 35")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE mod(a, 11) = 1 AND mod(b::int, 13) = 1 AND mod(c, 7) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS func_deps_stat (dependencies) ON (mod(a,11)), (mod(b::int, 13)), (mod(c, 7)) FROM functional_dependencies;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE mod(a, 11) = 1 AND mod(b::int, 13) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("35 35")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE mod(a, 11) = 1 AND mod(b::int, 13) = 1 AND mod(c, 7) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("5 5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE functional_dependencies;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS func_deps_stat;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO functional_dependencies (a, b, c, filler1) SELECT mod(i,100), mod(i,50), mod(i,25), i FROM generate_series(1,5000) s(i);",
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 51) AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 51) AND b IN (''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("4 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 2, 51, 52) AND b IN (''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("8 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 2, 51, 52) AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("4 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 26, 51, 76) AND b IN (''1'', ''26'') AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 26, 51, 76) AND b IN (''1'', ''26'') AND c IN (1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 2, 26, 27, 51, 52, 76, 77) AND b IN (''1'', ''2'', ''26'', ''27'') AND c IN (1, 2)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a = 1 OR a = 51) AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a = 1 OR a = 51) AND (b = ''1'' OR b = ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("4 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a = 1 OR a = 2 OR a = 51 OR a = 52) AND (b = ''1'' OR b = ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("8 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a = 1 OR b = ''1'') AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 51]) AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 51]) AND b = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("4 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 2, 51, 52]) AND b = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("8 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 26, 51, 76]) AND b = ANY (ARRAY[''1'', ''26'']) AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 26, 51, 76]) AND b = ANY (ARRAY[''1'', ''26'']) AND c = ANY (ARRAY[1])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 2, 26, 27, 51, 52, 76, 77]) AND b = ANY (ARRAY[''1'', ''2'', ''26'', ''27'']) AND c = ANY (ARRAY[1, 2])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a < ANY (ARRAY[1, 51]) AND b > ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2472 2400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a >= ANY (ARRAY[1, 51]) AND b <= ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1441 1250")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a <= ANY (ARRAY[1, 2, 51, 52]) AND b >= ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3909 2550")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 51) AND b = ALL (ARRAY[''1''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 51) AND b = ALL (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 2, 51, 52) AND b = ALL (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS func_deps_stat (dependencies) ON a, b, c FROM functional_dependencies;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dependencies FROM pg_stats_ext WHERE statistics_name = 'func_deps_stat';",
                    expected: Expected::Rows {
                        columns: &[Column("dependencies", PG_DEPENDENCIES)],
                        rows: &[
                            &[T(r#"{"3 => 4": 1.000000, "3 => 6": 1.000000, "4 => 6": 1.000000, "3, 4 => 6": 1.000000, "3, 6 => 4": 1.000000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 51) AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 51) AND b IN (''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 2, 51, 52) AND b IN (''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 2, 51, 52) AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 26, 51, 76) AND b IN (''1'', ''26'') AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 26, 51, 76) AND b IN (''1'', ''26'') AND c IN (1)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 2, 26, 27, 51, 52, 76, 77) AND b IN (''1'', ''2'', ''26'', ''27'') AND c IN (1, 2)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("400 400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a = 1 OR a = 51) AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("99 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a = 1 OR a = 51) AND (b = ''1'' OR b = ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("99 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a = 1 OR a = 2 OR a = 51 OR a = 52) AND (b = ''1'' OR b = ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("197 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a = 1 OR b = ''1'') AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 51]) AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 51]) AND b = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 2, 51, 52]) AND b = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 26, 51, 76]) AND b = ANY (ARRAY[''1'', ''26'']) AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 26, 51, 76]) AND b = ANY (ARRAY[''1'', ''26'']) AND c = ANY (ARRAY[1])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = ANY (ARRAY[1, 2, 26, 27, 51, 52, 76, 77]) AND b = ANY (ARRAY[''1'', ''2'', ''26'', ''27'']) AND c = ANY (ARRAY[1, 2])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("400 400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a < ANY (ARRAY[1, 51]) AND b > ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2472 2400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a >= ANY (ARRAY[1, 51]) AND b <= ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1441 1250")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a <= ANY (ARRAY[1, 2, 51, 52]) AND b >= ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3909 2550")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 51) AND b = ALL (ARRAY[''1''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 51) AND b = ALL (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a IN (1, 2, 51, 52) AND b = ALL (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE functional_dependencies ALTER COLUMN c TYPE numeric;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS func_deps_stat;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = 2 AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = 2 AND upper(b) = ''1'' AND (c + 1) = 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 102) AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 102) AND upper(b) IN (''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 4, 102, 104) AND upper(b) IN (''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 4, 102, 104) AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 52, 102, 152) AND upper(b) IN (''1'', ''26'') AND (c + 1) = 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 52, 102, 152) AND upper(b) IN (''1'', ''26'') AND (c + 1) IN (2)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 4, 52, 54, 102, 104, 152, 154) AND upper(b) IN (''1'', ''2'', ''26'', ''27'') AND (c + 1) IN (2, 3)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE ((a * 2) = 2 OR (a * 2) = 102) AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE ((a * 2) = 2 OR (a * 2) = 102) AND (upper(b) = ''1'' OR upper(b) = ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE ((a * 2) = 2 OR (a * 2) = 4 OR (a * 2) = 102 OR (a * 2) = 104) AND (upper(b) = ''1'' OR upper(b) = ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE ((a * 2) = 2 OR upper(b) = ''1'') AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 102]) AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 102]) AND upper(b) = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 4, 102, 104]) AND upper(b) = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 52, 102, 152]) AND upper(b) = ANY (ARRAY[''1'', ''26'']) AND (c + 1) = 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 52, 102, 152]) AND upper(b) = ANY (ARRAY[''1'', ''26'']) AND (c + 1) = ANY (ARRAY[2])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 4, 52, 54, 102, 104, 152, 154]) AND upper(b) = ANY (ARRAY[''1'', ''2'', ''26'', ''27'']) AND (c + 1) = ANY (ARRAY[2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) < ANY (ARRAY[2, 102]) AND upper(b) > ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("926 2400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) >= ANY (ARRAY[2, 102]) AND upper(b) <= ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1543 1250")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) <= ANY (ARRAY[2, 4, 102, 104]) AND upper(b) >= ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2229 2550")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 102) AND upper(b) = ALL (ARRAY[''1''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 102) AND upper(b) = ALL (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 4, 102, 104) AND upper(b) = ALL (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS func_deps_stat (dependencies) ON (a * 2), upper(b), (c + 1) FROM functional_dependencies;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dependencies FROM pg_stats_ext WHERE statistics_name = 'func_deps_stat';",
                    expected: Expected::Rows {
                        columns: &[Column("dependencies", PG_DEPENDENCIES)],
                        rows: &[
                            &[T(r#"{"-1 => -2": 1.000000, "-1 => -3": 1.000000, "-2 => -3": 1.000000, "-1, -2 => -3": 1.000000, "-1, -3 => -2": 1.000000}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = 2 AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = 2 AND upper(b) = ''1'' AND (c + 1) = 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 102) AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 102) AND upper(b) IN (''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 4, 102, 104) AND upper(b) IN (''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 4, 102, 104) AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 52, 102, 152) AND upper(b) IN (''1'', ''26'') AND (c + 1) = 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 52, 102, 152) AND upper(b) IN (''1'', ''26'') AND (c + 1) IN (2)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 4, 52, 54, 102, 104, 152, 154) AND upper(b) IN (''1'', ''2'', ''26'', ''27'') AND (c + 1) IN (2, 3)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("400 400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE ((a * 2) = 2 OR (a * 2) = 102) AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("99 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE ((a * 2) = 2 OR (a * 2) = 102) AND (upper(b) = ''1'' OR upper(b) = ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("99 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE ((a * 2) = 2 OR (a * 2) = 4 OR (a * 2) = 102 OR (a * 2) = 104) AND (upper(b) = ''1'' OR upper(b) = ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("197 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE ((a * 2) = 2 OR upper(b) = ''1'') AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 102]) AND upper(b) = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 102]) AND upper(b) = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 4, 102, 104]) AND upper(b) = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 52, 102, 152]) AND upper(b) = ANY (ARRAY[''1'', ''26'']) AND (c + 1) = 2');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 52, 102, 152]) AND upper(b) = ANY (ARRAY[''1'', ''26'']) AND (c + 1) = ANY (ARRAY[2])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) = ANY (ARRAY[2, 4, 52, 54, 102, 104, 152, 154]) AND upper(b) = ANY (ARRAY[''1'', ''2'', ''26'', ''27'']) AND (c + 1) = ANY (ARRAY[2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("400 400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) < ANY (ARRAY[2, 102]) AND upper(b) > ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2472 2400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) >= ANY (ARRAY[2, 102]) AND upper(b) <= ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1441 1250")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) <= ANY (ARRAY[2, 4, 102, 104]) AND upper(b) >= ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3909 2550")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 102) AND upper(b) = ALL (ARRAY[''1''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("2 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 102) AND upper(b) = ALL (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies WHERE (a * 2) IN (2, 4, 102, 104) AND upper(b) = ALL (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE functional_dependencies_multi ( a INTEGER, b INTEGER, c INTEGER, d INTEGER ) WITH (autovacuum_enabled = off);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO functional_dependencies_multi (a, b, c, d) SELECT mod(i,7), mod(i,7), mod(i,11), mod(i,11) FROM generate_series(1,5000) s(i);",
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies_multi;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE a = 0 AND b = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("102 714")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE 0 = a AND 0 = b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("102 714")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE c = 0 AND d = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("41 454")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE a = 0 AND b = 0 AND c = 0 AND d = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE 0 = a AND b = 0 AND 0 = c AND d = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS functional_dependencies_multi_1 (dependencies) ON a, b FROM functional_dependencies_multi;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS functional_dependencies_multi_2 (dependencies) ON c, d FROM functional_dependencies_multi;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE functional_dependencies_multi;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE a = 0 AND b = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("714 714")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE 0 = a AND 0 = b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("714 714")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE c = 0 AND d = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("454 454")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE a = 0 AND b = 0 AND c = 0 AND d = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("65 64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM functional_dependencies_multi WHERE 0 = a AND b = 0 AND 0 = c AND d = 0');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("65 64")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE functional_dependencies_multi;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE mcv_lists ( filler1 TEXT, filler2 NUMERIC, a INT, b VARCHAR, filler3 DATE, c INT, d TEXT, ia INT[] ) WITH (autovacuum_enabled = off);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mcv_lists (a, b, c, filler1) SELECT mod(i,37), mod(i,41), mod(i,43), mod(i,47) FROM generate_series(1,5000) s(i);",
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS mcv_lists_stats (mcv) ON a, b, c FROM mcv_lists;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("3 4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE mcv_lists;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS mcv_lists_stats;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mcv_lists (a, b, c, filler1) SELECT i, i, i, i FROM generate_series(1,1000) s(i);",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,7) = 1 AND mod(b::int,11) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 13")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,7) = 1 AND mod(b::int,11) = 1 AND mod(c,13) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS mcv_lists_stats (mcv) ON (mod(a,7)), (mod(b::int,11)), (mod(c,13)) FROM mcv_lists;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,7) = 1 AND mod(b::int,11) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("13 13")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,7) = 1 AND mod(b::int,11) = 1 AND mod(c,13) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE mcv_lists;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS mcv_lists_stats;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mcv_lists (a, b, c, ia, filler1) SELECT mod(i,100), mod(i,50), mod(i,25), array[mod(i,25)], i FROM generate_series(1,5000) s(i);",
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 = a AND ''1'' = b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < 1 AND b < ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 > a AND ''1'' > b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a <= 0 AND b <= ''0''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 0 >= a AND ''0'' >= b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < 5 AND b < ''1'' AND c < 5');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < 5 AND ''1'' > b AND 5 > c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a <= 4 AND b <= ''0'' AND c <= 4');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 4 >= a AND ''0'' >= b AND 4 >= c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 OR b = ''1'' OR c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("343 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 OR b = ''1'' OR c = 1 OR d IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("343 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IN (1, 2, 51, 52) AND b IN ( ''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("8 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IN (1, 2, 51, 52, NULL) AND b IN ( ''1'', ''2'', NULL)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("8 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = ANY (ARRAY[1, 2, 51, 52]) AND b = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("8 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = ANY (ARRAY[NULL, 1, 2, 51, 52]) AND b = ANY (ARRAY[''1'', ''2'', NULL])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("8 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a <= ANY (ARRAY[1, 2, 3]) AND b IN (''1'', ''2'', ''3'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("26 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a <= ANY (ARRAY[1, NULL, 2, 3]) AND b IN (''1'', ''2'', NULL, ''3'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("26 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < ALL (ARRAY[4, 5]) AND c > ANY (ARRAY[1, 2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("10 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < ALL (ARRAY[4, 5]) AND c > ANY (ARRAY[1, 2, 3, NULL])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("10 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < ALL (ARRAY[4, 5]) AND b IN (''1'', ''2'', ''3'') AND c > ANY (ARRAY[1, 2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < ALL (ARRAY[4, 5]) AND b IN (''1'', ''2'', NULL, ''3'') AND c > ANY (ARRAY[1, 2, NULL, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = ANY (ARRAY[4,5]) AND 4 = ANY(ia)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("4 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS mcv_lists_stats (mcv) ON a, b, c, ia FROM mcv_lists;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 = a AND ''1'' = b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < 1 AND b < ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 > a AND ''1'' > b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a <= 0 AND b <= ''0''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 0 >= a AND ''0'' >= b');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1'' AND c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < 5 AND b < ''1'' AND c < 5');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < 5 AND ''1'' > b AND 5 > c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a <= 4 AND b <= ''0'' AND c <= 4');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 4 >= a AND ''0'' >= b AND 4 >= c');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 OR b = ''1'' OR c = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 OR b = ''1'' OR c = 1 OR d IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 OR b = ''1'' OR c = 1 OR d IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IN (1, 2, 51, 52) AND b IN ( ''1'', ''2'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IN (1, 2, 51, 52, NULL) AND b IN ( ''1'', ''2'', NULL)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = ANY (ARRAY[1, 2, 51, 52]) AND b = ANY (ARRAY[''1'', ''2''])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = ANY (ARRAY[NULL, 1, 2, 51, 52]) AND b = ANY (ARRAY[''1'', ''2'', NULL])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a <= ANY (ARRAY[1, 2, 3]) AND b IN (''1'', ''2'', ''3'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("150 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a <= ANY (ARRAY[1, NULL, 2, 3]) AND b IN (''1'', ''2'', NULL, ''3'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("150 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < ALL (ARRAY[4, 5]) AND c > ANY (ARRAY[1, 2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < ALL (ARRAY[4, 5]) AND c > ANY (ARRAY[1, 2, 3, NULL])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < ALL (ARRAY[4, 5]) AND b IN (''1'', ''2'', ''3'') AND c > ANY (ARRAY[1, 2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a < ALL (ARRAY[4, 5]) AND b IN (''1'', ''2'', NULL, ''3'') AND c > ANY (ARRAY[1, 2, NULL, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = ANY (ARRAY[4,5]) AND 4 = ANY(ia)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("4 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mcv_lists ALTER COLUMN d TYPE VARCHAR(64);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.stxdmcv IS NOT NULL FROM pg_statistic_ext s, pg_statistic_ext_data d WHERE s.stxname = 'mcv_lists_stats' AND d.stxoid = s.oid;",
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
                    query: "ALTER TABLE mcv_lists ALTER COLUMN c TYPE numeric;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a = 1 AND b = ''1''');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE mcv_lists;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS mcv_lists_stats;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mcv_lists (a, b, c, filler1) SELECT i, i, i, i FROM generate_series(1,1000) s(i);",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 AND mod(b::int,10) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 = mod(a,20) AND 1 = mod(b::int,10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) < 1 AND mod(b::int,10) < 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("111 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 > mod(a,20) AND 1 > mod(b::int,10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("111 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 AND mod(b::int,10) = 1 AND mod(c,5) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 OR mod(b::int,10) = 1 OR mod(c,25) = 1 OR d IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("15 120")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) IN (1, 2, 51, 52, NULL) AND mod(b::int,10) IN ( 1, 2, NULL)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = ANY (ARRAY[1, 2, 51, 52]) AND mod(b::int,10) = ANY (ARRAY[1, 2])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) <= ANY (ARRAY[1, NULL, 2, 3]) AND mod(b::int,10) IN (1, 2, NULL, 3)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("11 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) < ALL (ARRAY[4, 5]) AND mod(b::int,10) IN (1, 2, 3) AND mod(c,5) > ANY (ARRAY[1, 2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS mcv_lists_stats_1 ON (mod(a,20)) FROM mcv_lists;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS mcv_lists_stats_2 ON (mod(b::int,10)) FROM mcv_lists;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS mcv_lists_stats_3 ON (mod(c,5)) FROM mcv_lists;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 AND mod(b::int,10) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("5 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 = mod(a,20) AND 1 = mod(b::int,10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("5 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) < 1 AND mod(b::int,10) < 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("5 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 > mod(a,20) AND 1 > mod(b::int,10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("5 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 AND mod(b::int,10) = 1 AND mod(c,5) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 OR mod(b::int,10) = 1 OR mod(c,25) = 1 OR d IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("149 120")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) IN (1, 2, 51, 52, NULL) AND mod(b::int,10) IN ( 1, 2, NULL)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("20 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = ANY (ARRAY[1, 2, 51, 52]) AND mod(b::int,10) = ANY (ARRAY[1, 2])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("20 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) <= ANY (ARRAY[1, NULL, 2, 3]) AND mod(b::int,10) IN (1, 2, NULL, 3)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("116 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) < ALL (ARRAY[4, 5]) AND mod(b::int,10) IN (1, 2, 3) AND mod(c,5) > ANY (ARRAY[1, 2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("12 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS mcv_lists_stats_1;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS mcv_lists_stats_2;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS mcv_lists_stats_3;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS mcv_lists_stats (mcv) ON (mod(a,20)), (mod(b::int,10)), (mod(c,5)) FROM mcv_lists;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 AND mod(b::int,10) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 = mod(a,20) AND 1 = mod(b::int,10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) < 1 AND mod(b::int,10) < 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE 1 > mod(a,20) AND 1 > mod(b::int,10)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 AND mod(b::int,10) = 1 AND mod(c,5) = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 OR mod(b::int,10) = 1 OR mod(c,25) = 1 OR d IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("105 120")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) IN (1, 2, 51, 52, NULL) AND mod(b::int,10) IN ( 1, 2, NULL)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = ANY (ARRAY[1, 2, 51, 52]) AND mod(b::int,10) = ANY (ARRAY[1, 2])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) <= ANY (ARRAY[1, NULL, 2, 3]) AND mod(b::int,10) IN (1, 2, NULL, 3)');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("150 150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) < ALL (ARRAY[4, 5]) AND mod(b::int,10) IN (1, 2, 3) AND mod(c,5) > ANY (ARRAY[1, 2, 3])');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("100 100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE mod(a,20) = 1 OR mod(b::int,10) = 1 OR mod(c,5) = 1 OR d IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("200 200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE mcv_lists;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP STATISTICS mcv_lists_stats;",
                    expected: Expected::Tag("DROP STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mcv_lists (a, b, c, filler1) SELECT (CASE WHEN mod(i,100) = 1 THEN NULL ELSE mod(i,100) END), (CASE WHEN mod(i,50) = 1 THEN NULL ELSE mod(i,50) END), (CASE WHEN mod(i,25) = 1 THEN NULL ELSE mod(i,25) END), i FROM generate_series(1,5000) s(i);",
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IS NULL AND b IS NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IS NULL AND b IS NULL AND c IS NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IS NULL AND b IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("49 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IS NOT NULL AND b IS NULL AND c IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("95 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IN (0, 1) AND b IN (''0'', ''1'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS mcv_lists_stats (mcv) ON a, b, c FROM mcv_lists;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE mcv_lists;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IS NULL AND b IS NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IS NULL AND b IS NULL AND c IS NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IS NULL AND b IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IS NOT NULL AND b IS NULL AND c IS NOT NULL');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("1 0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT check_estimated_rows('SELECT * FROM mcv_lists WHERE a IN (0, 1) AND b IN (''0'', ''1'')');",
                    expected: Expected::Rows {
                        columns: &[Column("check_estimated_rows", TEXT)],
                        rows: &[
                            &[T("50 50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ndistinct statistics on a hash join's inner keys give its bucket size, from stats_ext.sql",
            set_up_script: &[
                "CREATE TABLE sb_1 (x NUMERIC, y NUMERIC, z NUMERIC);",
                "INSERT INTO sb_1 SELECT gs % 10, gs % 10, gs % 10 FROM generate_series(1, 1e4) AS gs;",
                "CREATE TABLE sb_2 (x NUMERIC, y NUMERIC, z NUMERIC, payload TEXT);",
                "INSERT INTO sb_2 SELECT gs % 49, gs % 51, gs % 73, 'abc' || gs FROM generate_series(1, 1e4) AS gs;",
                "ANALYZE sb_1, sb_2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM sb_1 a, sb_2 b WHERE a.x = b.x AND a.y = b.y AND a.z = b.z;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Merge Join"#)],
                            &[T(r#"  Merge Cond: ((a.z = b.z) AND (a.x = b.x) AND (a.y = b.y))"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: a.z, a.x, a.y"#)],
                            &[T(r#"        ->  Seq Scan on sb_1 a"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: b.z, b.x, b.y"#)],
                            &[T(r#"        ->  Seq Scan on sb_2 b"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE STATISTICS extstat_sb_2 (ndistinct) ON x, y, z FROM sb_2;",
                    expected: Expected::Tag("CREATE STATISTICS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE sb_2;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM sb_1 a, sb_2 b WHERE a.x = b.x AND a.y = b.y AND a.z = b.z;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Hash Join"#)],
                            &[T(r#"  Hash Cond: ((a.x = b.x) AND (a.y = b.y) AND (a.z = b.z))"#)],
                            &[T(r#"  ->  Seq Scan on sb_1 a"#)],
                            &[T(r#"  ->  Hash"#)],
                            &[T(r#"        ->  Seq Scan on sb_2 b"#)],
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
fn test_statistics_kept_out_of_line() {
    run_scripts(&[
        ScriptTest {
            name: "statistics too large for a tree's node are kept out of line",
            set_up_script: &[
                "CREATE TABLE wide_stats (t TEXT, u TEXT, v TEXT);",
                "INSERT INTO wide_stats SELECT repeat(md5((g % 50)::text), 30), repeat(md5(g::text), 30), repeat(md5((g % 50)::text), 20) FROM generate_series(1, 1000) g;",
                "CREATE STATISTICS wide_stats_mcv (mcv) ON t, v FROM wide_stats;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE wide_stats;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, array_length(most_common_vals::text::text[], 1) AS mcvs, array_length(histogram_bounds::text::text[], 1) AS bounds FROM pg_stats WHERE tablename = 'wide_stats' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("mcvs", INT4), Column("bounds", INT4)],
                        rows: &[
                            &[T("t"), T("50"), Null],
                            &[T("u"), Null, T("101")],
                            &[T("v"), T("50"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(length(values::text)) FROM pg_mcv_list_items((SELECT stxdmcv FROM pg_statistic_ext_data JOIN pg_statistic_ext ON stxoid = oid WHERE stxname = 'wide_stats_mcv'));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("50"), T("80150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_statistics_on_function_expressions() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE computes statistics on expressions that call SQL and PL/pgSQL functions",
            set_up_script: &[
                "CREATE TABLE es (id INT PRIMARY KEY, a INT, b INT);",
                "INSERT INTO es SELECT i, i % 10, i % 7 FROM generate_series(1, 1000) i;",
                "CREATE FUNCTION fpl(x INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN x * 2; END $$;",
                "CREATE FUNCTION fsql(x INT) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT x + 1 $$;",
                "CREATE STATISTICS s1 ON (fpl(a)) FROM es;",
                "CREATE STATISTICS s2 ON (fsql(a)), b FROM es;",
                "CREATE STATISTICS s3 (ndistinct, dependencies) ON (fpl(a)), (fsql(b)) FROM es;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE es;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statistics_name, expr, null_frac, n_distinct, most_common_vals::TEXT FROM pg_stats_ext_exprs WHERE tablename = 'es' ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("statistics_name", NAME), Column("expr", TEXT), Column("null_frac", FLOAT4), Column("n_distinct", FLOAT4), Column("most_common_vals", TEXT)],
                        rows: &[
                            &[T("s1"), T("fpl(a)"), T("0"), T("10"), T("{0,2,4,6,8,10,12,14,16,18}")],
                            &[T("s2"), T("fsql(a)"), T("0"), T("10"), T("{1,2,3,4,5,6,7,8,9,10}")],
                            &[T("s3"), T("fpl(a)"), T("0"), T("10"), T("{0,2,4,6,8,10,12,14,16,18}")],
                            &[T("s3"), T("fsql(b)"), T("0"), T("7"), T("{2,3,4,5,6,7,1}")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statistics_name, n_distinct::TEXT, dependencies::TEXT FROM pg_stats_ext WHERE tablename = 'es' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("statistics_name", NAME), Column("n_distinct", TEXT), Column("dependencies", TEXT)],
                        rows: &[
                            &[T("s1"), Null, Null],
                            &[T("s2"), T("{\"3, -1\": 70}"), Null],
                            &[T("s3"), T("{\"-1, -2\": 70}"), Null],
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
fn test_statistics_expressions_evaluated_as_postgres_does() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE evaluates a statistics object's expressions for its data and again for each expression's statistics, over the table and its inheritance tree",
            set_up_script: &[
                "CREATE FUNCTION f(i INT) RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'f(%)', i; RETURN i; END $$;",
                "CREATE TABLE p (a INT, b INT);",
                "CREATE TABLE c () INHERITS (p);",
                "INSERT INTO p VALUES (1, 10);",
                "INSERT INTO c VALUES (2, 20);",
                "CREATE STATISTICS s ON (f(a)), (f(b)) FROM p;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE p;",
                    notices: &[Diagnostic { code: "00000", message: "f(1)", ..N }, Diagnostic { code: "00000", message: "f(10)", ..N }, Diagnostic { code: "00000", message: "f(1)", ..N }, Diagnostic { code: "00000", message: "f(10)", ..N }, Diagnostic { code: "00000", message: "f(1)", ..N }, Diagnostic { code: "00000", message: "f(10)", ..N }, Diagnostic { code: "00000", message: "f(2)", ..N }, Diagnostic { code: "00000", message: "f(20)", ..N }, Diagnostic { code: "00000", message: "f(1)", ..N }, Diagnostic { code: "00000", message: "f(2)", ..N }, Diagnostic { code: "00000", message: "f(10)", ..N }, Diagnostic { code: "00000", message: "f(20)", ..N }],
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stats_ext_exprs WHERE statistics_name = 's';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
fn test_extended_mcv_lists_of_user_types() {
    run_scripts(&[
        ScriptTest {
            name: "extended statistics keep the most common combinations of enum and composite values, which read renamed labels",
            set_up_script: &[
                "CREATE TYPE mcv_mood AS ENUM ('sad', 'ok', 'happy');",
                "CREATE TYPE mcv_pair AS (a INTEGER, b TEXT);",
                "CREATE TABLE mcv_user_types (m mcv_mood, p mcv_pair, n INTEGER);",
                "INSERT INTO mcv_user_types SELECT (ARRAY['sad', 'ok', 'happy']::mcv_mood[])[1 + i % 3], ROW(i % 2, 'x')::mcv_pair, i % 3 FROM generate_series(0, 299) i;",
                "CREATE STATISTICS mcv_user_types_stats (mcv) ON m, p, n FROM mcv_user_types;",
                "ANALYZE mcv_user_types;",
                "CREATE FUNCTION est(q TEXT) RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE j JSON; BEGIN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j; RETURN j->0->'Plan'->>'Plan Rows'; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT m.index, m.values, m.nulls, m.frequency FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid, pg_mcv_list_items(d.stxdmcv) m WHERE s.stxname = 'mcv_user_types_stats' ORDER BY m.index;",
                    expected: Expected::Rows {
                        columns: &[Column("index", INT4), Column("values", TEXT_ARRAY), Column("nulls", BOOL_ARRAY), Column("frequency", FLOAT8)],
                        rows: &[
                            &[T("0"), T(r#"{sad,"(0,x)",0}"#), T("{f,f,f}"), T("0.16666666666666666")],
                            &[T("1"), T(r#"{sad,"(1,x)",0}"#), T("{f,f,f}"), T("0.16666666666666666")],
                            &[T("2"), T(r#"{ok,"(0,x)",1}"#), T("{f,f,f}"), T("0.16666666666666666")],
                            &[T("3"), T(r#"{ok,"(1,x)",1}"#), T("{f,f,f}"), T("0.16666666666666666")],
                            &[T("4"), T(r#"{happy,"(0,x)",2}"#), T("{f,f,f}"), T("0.16666666666666666")],
                            &[T("5"), T(r#"{happy,"(1,x)",2}"#), T("{f,f,f}"), T("0.16666666666666666")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE mcv_mood RENAME VALUE 'ok' TO 'fine';",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT most_common_vals FROM pg_stats_ext WHERE statistics_name = 'mcv_user_types_stats';",
                    expected: Expected::Rows {
                        columns: &[Column("most_common_vals", TEXT_ARRAY)],
                        rows: &[
                            &[T(r#"{{sad,"(0,x)",0},{sad,"(1,x)",0},{fine,"(0,x)",1},{fine,"(1,x)",1},{happy,"(0,x)",2},{happy,"(1,x)",2}}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM mcv_user_types WHERE m = ''fine'' AND n = 1'), est('SELECT * FROM mcv_user_types WHERE p = ROW(1, ''x'')::mcv_pair AND n = 1');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("100"), T("50")],
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
