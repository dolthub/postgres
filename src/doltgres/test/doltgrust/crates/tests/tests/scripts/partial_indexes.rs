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
use harness::script::{A, Column, Diagnostic, E, Expected, PRIMARY_KEY_RETURNED, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_partial_indexes() {
    run_scripts(&[
        ScriptTest {
            name: "partial indexes of select.sql and create_index.sql",
            set_up_script: &[
                "CREATE TABLE onek2 (unique1 INT4, unique2 INT4, stringu1 NAME);",
                "INSERT INTO onek2 (unique1) VALUES (0), (1), (2), (3), (4), (5), (6), (7);",
                "INSERT INTO onek2 (unique1) SELECT unique1 + 8 FROM onek2;",
                "INSERT INTO onek2 (unique1) SELECT unique1 + 16 FROM onek2;",
                "INSERT INTO onek2 (unique1) SELECT unique1 + 32 FROM onek2;",
                "INSERT INTO onek2 (unique1) SELECT unique1 + 64 FROM onek2;",
                "INSERT INTO onek2 (unique1) SELECT unique1 + 128 FROM onek2;",
                "INSERT INTO onek2 (unique1) SELECT unique1 + 256 FROM onek2;",
                "INSERT INTO onek2 (unique1) SELECT unique1 + 512 FROM onek2;",
                "DELETE FROM onek2 WHERE unique1 >= 1000;",
                "UPDATE onek2 SET unique2 = 999 - unique1, stringu1 = substr('ABCDEFGHIJKLMNOPQRSTUVWXYZ', unique1 % 26 + 1, 1) || substr('ABCDEFGHIJKLMNOPQRSTUVWXYZ', (unique1 / 26) % 26 + 1, 1) || 'AAAA';",
                "CREATE INDEX onek2_u1_prtl ON onek2 USING btree(unique1 int4_ops) WHERE unique1 < 20 OR unique1 > 980;",
                "CREATE INDEX onek2_u2_prtl ON onek2 USING btree(unique2 int4_ops) WHERE stringu1 < 'B';",
                "CREATE INDEX onek2_stu1_prtl ON onek2 USING btree(stringu1 name_ops) WHERE onek2.stringu1 >= 'J' AND onek2.stringu1 < 'K';",
                "ANALYZE onek2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan TO off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan TO off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_sort TO off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT onek2.* FROM onek2 WHERE onek2.unique1 < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("unique1", INT4), Column("unique2", INT4), Column("stringu1", NAME)],
                        rows: &[
                            &[T("0"), T("999"), T("AAAAAA")],
                            &[T("1"), T("998"), T("BAAAAA")],
                            &[T("2"), T("997"), T("CAAAAA")],
                            &[T("3"), T("996"), T("DAAAAA")],
                            &[T("4"), T("995"), T("EAAAAA")],
                            &[T("5"), T("994"), T("FAAAAA")],
                            &[T("6"), T("993"), T("GAAAAA")],
                            &[T("7"), T("992"), T("HAAAAA")],
                            &[T("8"), T("991"), T("IAAAAA")],
                            &[T("9"), T("990"), T("JAAAAA")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT onek2.unique1, onek2.stringu1 FROM onek2 WHERE onek2.unique1 < 20 ORDER BY unique1 USING >;",
                    expected: Expected::Rows {
                        columns: &[Column("unique1", INT4), Column("stringu1", NAME)],
                        rows: &[
                            &[T("19"), T("TAAAAA")],
                            &[T("18"), T("SAAAAA")],
                            &[T("17"), T("RAAAAA")],
                            &[T("16"), T("QAAAAA")],
                            &[T("15"), T("PAAAAA")],
                            &[T("14"), T("OAAAAA")],
                            &[T("13"), T("NAAAAA")],
                            &[T("12"), T("MAAAAA")],
                            &[T("11"), T("LAAAAA")],
                            &[T("10"), T("KAAAAA")],
                            &[T("9"), T("JAAAAA")],
                            &[T("8"), T("IAAAAA")],
                            &[T("7"), T("HAAAAA")],
                            &[T("6"), T("GAAAAA")],
                            &[T("5"), T("FAAAAA")],
                            &[T("4"), T("EAAAAA")],
                            &[T("3"), T("DAAAAA")],
                            &[T("2"), T("CAAAAA")],
                            &[T("1"), T("BAAAAA")],
                            &[T("0"), T("AAAAAA")],
                        ],
                        tag: "SELECT 20",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT onek2.unique1, onek2.stringu1 FROM onek2 WHERE onek2.unique1 > 980;",
                    expected: Expected::Rows {
                        columns: &[Column("unique1", INT4), Column("stringu1", NAME)],
                        rows: &[
                            &[T("981"), T("TLAAAA")],
                            &[T("982"), T("ULAAAA")],
                            &[T("983"), T("VLAAAA")],
                            &[T("984"), T("WLAAAA")],
                            &[T("985"), T("XLAAAA")],
                            &[T("986"), T("YLAAAA")],
                            &[T("987"), T("ZLAAAA")],
                            &[T("988"), T("AMAAAA")],
                            &[T("989"), T("BMAAAA")],
                            &[T("990"), T("CMAAAA")],
                            &[T("991"), T("DMAAAA")],
                            &[T("992"), T("EMAAAA")],
                            &[T("993"), T("FMAAAA")],
                            &[T("994"), T("GMAAAA")],
                            &[T("995"), T("HMAAAA")],
                            &[T("996"), T("IMAAAA")],
                            &[T("997"), T("JMAAAA")],
                            &[T("998"), T("KMAAAA")],
                            &[T("999"), T("LMAAAA")],
                        ],
                        tag: "SELECT 19",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT onek2.unique1, onek2.stringu1 FROM onek2 WHERE onek2.unique1 > 980;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using onek2_u1_prtl on onek2"#)],
                            &[T(r#"  Index Cond: (unique1 > 980)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_bitmapscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_sort;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM onek2 WHERE unique2 = 11 AND stringu1 = 'AMAAAA';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using onek2_u2_prtl on onek2"#)],
                            &[T(r#"  Index Cond: (unique2 = 11)"#)],
                            &[T(r#"  Filter: (stringu1 = 'AMAAAA'::name)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onek2 WHERE unique2 = 11 AND stringu1 = 'AMAAAA';",
                    expected: Expected::Rows {
                        columns: &[Column("unique1", INT4), Column("unique2", INT4), Column("stringu1", NAME)],
                        rows: &[
                            &[T("988"), T("11"), T("AMAAAA")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF, ANALYZE ON, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM onek2 WHERE unique2 = 11 AND stringu1 = 'AMAAAA';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using onek2_u2_prtl on onek2 (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  Index Cond: (unique2 = 11)"#)],
                            &[T(r#"  Filter: (stringu1 = 'AMAAAA'::name)"#)],
                            &[T(r#"  Index Searches: 1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 = 'AMAAAA';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using onek2_u2_prtl on onek2"#)],
                            &[T(r#"  Index Cond: (unique2 = 11)"#)],
                            &[T(r#"  Filter: (stringu1 = 'AMAAAA'::name)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 = 'AMAAAA';",
                    expected: Expected::Rows {
                        columns: &[Column("unique2", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM onek2 WHERE unique2 = 11 AND stringu1 < 'B';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using onek2_u2_prtl on onek2"#)],
                            &[T(r#"  Index Cond: (unique2 = 11)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM onek2 WHERE unique2 = 11 AND stringu1 < 'B';",
                    expected: Expected::Rows {
                        columns: &[Column("unique1", INT4), Column("unique2", INT4), Column("stringu1", NAME)],
                        rows: &[
                            &[T("988"), T("11"), T("AMAAAA")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 < 'B';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Only Scan using onek2_u2_prtl on onek2"#)],
                            &[T(r#"  Index Cond: (unique2 = 11)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 < 'B';",
                    expected: Expected::Rows {
                        columns: &[Column("unique2", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 < 'B' FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  ->  Index Scan using onek2_u2_prtl on onek2"#)],
                            &[T(r#"        Index Cond: (unique2 = 11)"#)],
                            &[T(r#"        Filter: (stringu1 < 'B'::name)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 < 'B' FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("unique2", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 < 'C';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on onek2"#)],
                            &[T(r#"  Filter: ((stringu1 < 'C'::name) AND (unique2 = 11))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 < 'C';",
                    expected: Expected::Rows {
                        columns: &[Column("unique2", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan TO off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 < 'B';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Bitmap Heap Scan on onek2"#)],
                            &[T(r#"  Recheck Cond: ((unique2 = 11) AND (stringu1 < 'B'::name))"#)],
                            &[T(r#"  ->  Bitmap Index Scan on onek2_u2_prtl"#)],
                            &[T(r#"        Index Cond: (unique2 = 11)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unique2 FROM onek2 WHERE unique2 = 11 AND stringu1 < 'B';",
                    expected: Expected::Rows {
                        columns: &[Column("unique2", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_indexscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT unique1, unique2 FROM onek2 WHERE (unique2 = 11 OR unique1 = 0) AND stringu1 < 'B';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Bitmap Heap Scan on onek2"#)],
                            &[T(r#"  Recheck Cond: (((unique2 = 11) AND (stringu1 < 'B'::name)) OR (unique1 = 0))"#)],
                            &[T(r#"  Filter: (stringu1 < 'B'::name)"#)],
                            &[T(r#"  ->  BitmapOr"#)],
                            &[T(r#"        ->  Bitmap Index Scan on onek2_u2_prtl"#)],
                            &[T(r#"              Index Cond: (unique2 = 11)"#)],
                            &[T(r#"        ->  Bitmap Index Scan on onek2_u1_prtl"#)],
                            &[T(r#"              Index Cond: (unique1 = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("Doltgrust counts the pages of a tree of the rows a transaction sees, 7 for onek2 and 1 for each partial index, where Postgres counts 17 and 2 with the dead row versions this script's UPDATE left and each index's metapage, so the costs choose another bitmap plan"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unique1, unique2 FROM onek2 WHERE (unique2 = 11 OR unique1 = 0) AND stringu1 < 'B';",
                    expected: Expected::Rows {
                        columns: &[Column("unique1", INT4), Column("unique2", INT4)],
                        rows: &[
                            &[T("0"), T("999")],
                            &[T("988"), T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT unique1, unique2 FROM onek2 WHERE (unique2 = 11 AND stringu1 < 'B') OR unique1 = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Bitmap Heap Scan on onek2"#)],
                            &[T(r#"  Recheck Cond: (((unique2 = 11) AND (stringu1 < 'B'::name)) OR (unique1 = 0))"#)],
                            &[T(r#"  ->  BitmapOr"#)],
                            &[T(r#"        ->  Bitmap Index Scan on onek2_u2_prtl"#)],
                            &[T(r#"              Index Cond: (unique2 = 11)"#)],
                            &[T(r#"        ->  Bitmap Index Scan on onek2_u1_prtl"#)],
                            &[T(r#"              Index Cond: (unique1 = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unique1, unique2 FROM onek2 WHERE (unique2 = 11 AND stringu1 < 'B') OR unique1 = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("unique1", INT4), Column("unique2", INT4)],
                        rows: &[
                            &[T("0"), T("999")],
                            &[T("988"), T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT stringu1 FROM onek2 WHERE stringu1 >= 'J' AND stringu1 < 'K' AND stringu1 > 'JA';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Bitmap Heap Scan on onek2"#)],
                            &[T(r#"  Recheck Cond: ((stringu1 > 'JA'::name) AND (stringu1 < 'K'::name))"#)],
                            &[T(r#"  ->  Bitmap Index Scan on onek2_stu1_prtl"#)],
                            &[T(r#"        Index Cond: (stringu1 > 'JA'::name)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("every page of our tables counts as all-visible, so the index-only scan costs less than the bitmap heap scan PG18 chooses"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('onek2_u1_prtl'::regclass), pg_get_indexdef('onek2_stu1_prtl'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT), Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T("CREATE INDEX onek2_u1_prtl ON public.onek2 USING btree (unique1) WHERE ((unique1 < 20) OR (unique1 > 980))"), T("CREATE INDEX onek2_stu1_prtl ON public.onek2 USING btree (stringu1) WHERE ((stringu1 >= 'J'::name) AND (stringu1 < 'K'::name))")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE bitmap_split_or (a INT NOT NULL, b INT NOT NULL, c INT NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or VALUES (1, 1, 1), (1, 1, 2), (1, 1, 3), (1, 1, 4), (1, 1, 5), (1, 1, 6), (1, 1, 7), (1, 1, 8);",
                    expected: Expected::Tag("INSERT 0 8"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or SELECT a, b, c + 8 FROM bitmap_split_or;",
                    expected: Expected::Tag("INSERT 0 8"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or SELECT a, b, c + 16 FROM bitmap_split_or;",
                    expected: Expected::Tag("INSERT 0 16"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or SELECT a, b, c + 32 FROM bitmap_split_or;",
                    expected: Expected::Tag("INSERT 0 32"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or SELECT a, b, c + 64 FROM bitmap_split_or;",
                    expected: Expected::Tag("INSERT 0 64"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or SELECT a, b, c + 128 FROM bitmap_split_or;",
                    expected: Expected::Tag("INSERT 0 128"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or SELECT a, b, c + 256 FROM bitmap_split_or;",
                    expected: Expected::Tag("INSERT 0 256"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or SELECT a, b, c + 512 FROM bitmap_split_or;",
                    expected: Expected::Tag("INSERT 0 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM bitmap_split_or WHERE c > 1000;",
                    expected: Expected::Tag("DELETE 24"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bitmap_split_or SELECT c, 2, 2 FROM bitmap_split_or;",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM ANALYZE bitmap_split_or;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_b_partial_1_idx ON bitmap_split_or (b) WHERE a = 1;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_b_partial_2_idx ON bitmap_split_or (b) WHERE a = 2;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bitmap_split_or WHERE (a = 1 OR a = 2) AND b = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Bitmap Heap Scan on bitmap_split_or"#)],
                            &[T(r#"  Recheck Cond: (((b = 2) AND (a = 1)) OR ((b = 2) AND (a = 2)))"#)],
                            &[T(r#"  ->  BitmapOr"#)],
                            &[T(r#"        ->  Bitmap Index Scan on t_b_partial_1_idx"#)],
                            &[T(r#"              Index Cond: (b = 2)"#)],
                            &[T(r#"        ->  Bitmap Index Scan on t_b_partial_2_idx"#)],
                            &[T(r#"              Index Cond: (b = 2)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bitmap_split_or WHERE (a = 1 OR a = 2) AND b = 2;",
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
                    query: "DROP INDEX t_b_partial_1_idx;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX t_b_partial_2_idx;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pe (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pe_bad1 ON pe (a) WHERE random() > 0.5;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "functions in index predicate must be marked IMMUTABLE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pe_bad2 ON pe (a) WHERE count(b) > 0;",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate functions are not allowed in index predicates", position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pe_bad3 ON pe (a) WHERE b IN (SELECT 1);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use subquery in index predicate", position: 40, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pe_bad4 ON pe (a) WHERE b;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of WHERE must be type boolean, not type integer", position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pe_ok ON pe (a) WHERE b IS NOT NULL;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass, pg_get_expr(indpred, indrelid) FROM pg_index WHERE indrelid = 'pe'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("pg_get_expr", TEXT)],
                        rows: &[
                            &[T("pe_ok"), T("(b IS NOT NULL)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "partial unique indexes over equal rows, rows moving into and out of predicates, and the predicates queries imply",
            set_up_script: &[
                "CREATE TABLE ku (a INT, b INT);",
                "INSERT INTO ku VALUES (1, 1), (1, 0), (1, 0), (2, 1), (2, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX ku_a ON ku (a) WHERE b = 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"could not create unique index "ku_a""#, detail: "Key (a)=(2) is duplicated.", schema: "public", table: "ku", constraint: "ku_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ku WHERE a = 2;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX ku_a ON ku (a) WHERE b = 1;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (1, 0), (2, 0), (2, 0);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (1, 1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "ku_a""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "ku", constraint: "ku_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (2, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (2, 1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "ku_a""#, detail: "Key (a)=(2) already exists.", schema: "public", table: "ku", constraint: "ku_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ku SET b = 1 WHERE a = 1 AND b = 0;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "ku_a""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "ku", constraint: "ku_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ku SET b = 0 WHERE a = 2 AND b = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ku SET b = 1 WHERE a = 2 AND b = 0;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "ku_a""#, detail: "Key (a)=(2) already exists.", schema: "public", table: "ku", constraint: "ku_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, count(*) FROM ku GROUP BY a, b ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("0"), T("3")],
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("0"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM ku WHERE a = 1 AND b = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using ku_a on ku"#)],
                            &[T(r#"  Index Cond: (a = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM ku WHERE a = 1 AND b = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM ku WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on ku"#)],
                            &[T(r#"  Disabled: true"#)],
                            &[T(r#"  Filter: (a = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dp (id INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX dp_v ON dp (v) WHERE v > 10;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX dp_u ON dp (v) WHERE v > 100;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dp VALUES (1, 5), (2, 20), (3, 200);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dp SET v = 15 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dp SET v = 1 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dp SET v = 300 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dp SET v = 50 WHERE id = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id, v FROM dp WHERE v > 10 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using dp_v on dp"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan using dp_v on dp"])),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dp WHERE v > 10 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("3"), T("50")],
                            &[T("1"), T("300")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dp WHERE v > 100;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("300")],
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
                    query: "INSERT INTO dp VALUES (1, 30);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dp WHERE v > 10 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("30")],
                            &[T("3"), T("50")],
                            &[T("1"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dp SET v = 2 WHERE v = 30;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dp WHERE v > 10 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("3"), T("50")],
                            &[T("1"), T("300")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dp SET v = 300 WHERE v = 2;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "dp_u""#, detail: "Key (v)=(300) already exists.", schema: "public", table: "dp", constraint: "dp_u", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dp ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("300")],
                            &[T("2"), T("1")],
                            &[T("3"), T("50")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ex (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 8);",
                    expected: Expected::Tag("INSERT 0 8"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex SELECT a + 8, b + 8 FROM ex;",
                    expected: Expected::Tag("INSERT 0 8"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex SELECT a + 16, b + 16 FROM ex;",
                    expected: Expected::Tag("INSERT 0 16"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex SELECT a + 32, b + 32 FROM ex;",
                    expected: Expected::Tag("INSERT 0 32"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex SELECT a + 64, b + 64 FROM ex;",
                    expected: Expected::Tag("INSERT 0 64"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex SELECT a + 128, b + 128 FROM ex;",
                    expected: Expected::Tag("INSERT 0 128"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex SELECT a + 256, b + 256 FROM ex;",
                    expected: Expected::Tag("INSERT 0 256"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex SELECT a + 512, b + 512 FROM ex;",
                    expected: Expected::Tag("INSERT 0 512"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex SELECT a + 1024, b + 1024 FROM ex;",
                    expected: Expected::Tag("INSERT 0 1024"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ex_a_small ON ex (a) WHERE b < 100;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE ex;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM ex WHERE a = 5 AND b < 100;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Only Scan using ex_a_small on ex"#)],
                            &[T(r#"  Index Cond: (a = 5)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM ex WHERE a = 5 AND b < 50;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using ex_a_small on ex"#)],
                            &[T(r#"  Index Cond: (a = 5)"#)],
                            &[T(r#"  Filter: (b < 50)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM ex WHERE a = 5 AND b < 200;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on ex"#)],
                            &[T(r#"  Filter: ((b < 200) AND (a = 5))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM ex WHERE a = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on ex"#)],
                            &[T(r#"  Filter: (a = 5)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM ex WHERE a = 5 AND b < 50;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge of both branches' changes to a table with a partial index",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "CREATE INDEX t_v ON t (v) WHERE v > 10;",
                "INSERT INTO t VALUES (1, 5), (2, 20);",
                "SELECT DOLT_COMMIT('-Am', 'base');",
                "SELECT DOLT_CHECKOUT('-b', 'other');",
                "INSERT INTO t VALUES (3, 30), (4, 1);",
                "UPDATE t SET v = 25 WHERE id = 1;",
                "SELECT DOLT_COMMIT('-am', 'other rows');",
                "SELECT DOLT_CHECKOUT('main');",
                "INSERT INTO t VALUES (5, 50), (6, 2);",
                "UPDATE t SET v = 3 WHERE id = 2;",
                "SELECT DOLT_COMMIT('-am', 'main rows');",
                "SELECT DOLT_MERGE('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM t WHERE v > 10 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("25")],
                            &[T("3"), T("30")],
                            &[T("5"), T("50")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE v > 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
    ]);
}
