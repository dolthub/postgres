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
fn test_partitioned_row_triggers() {
    run_scripts(&[
        ScriptTest {
            name: "row triggers of partitioned tables fire on their partitions",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "INSERT INTO pt1 VALUES (1, 'x');",
                "CREATE TRIGGER nochange BEFORE UPDATE ON pt FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt2 VALUES (11, 'y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE pt1 SET b = b;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt2 SET b = b;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt1 SET b = 'z';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt DISABLE TRIGGER nochange;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt2 SET b = b;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass::text, tgparentid <> 0, tgenabled::text FROM pg_trigger WHERE tgname = 'nochange' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", TEXT), Column("?column?", BOOL), Column("tgenabled", TEXT)],
                        rows: &[
                            &[T("pt"), T("f"), T("D")],
                            &[T("pt1"), T("t"), T("D")],
                            &[T("pt2"), T("t"), T("D")],
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
fn test_partition_tuple_routing() {
    run_scripts(&[
        ScriptTest {
            name: "rows inserted into list partitioned tables go to their partitions",
            set_up_script: &[
                "CREATE TABLE l (a INT, b TEXT) PARTITION BY LIST (a);",
                "CREATE TABLE l1 PARTITION OF l FOR VALUES IN (1, 2);",
                "CREATE TABLE l2 PARTITION OF l FOR VALUES IN (3, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO l VALUES (1, 'one'), (3, 'three'), (NULL, 'null');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO l VALUES (5, 'five');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "l" found for row"#, detail: "Partition key of the failing row contains (a) = (5).", schema: "public", table: "l", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM l1 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("one")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM l2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("3"), T("three")], &[Null, T("null")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO l VALUES (2, 'two') RETURNING tableoid::regclass::text, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("l1"), T("2"), T("two")]],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO l1 VALUES (3, 'bad');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "l1" violates partition constraint"#, detail: "Failing row contains (3, bad).", schema: "public", table: "l1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO l1 VALUES (2, 'ok') RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("2"), T("ok")]],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE l1 SET a = 4 WHERE b = 'ok';",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "l1" violates partition constraint"#, detail: "Failing row contains (4, ok).", schema: "public", table: "l1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY l FROM STDIN;",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "partition-routing.tsv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM l1 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("copy1")], &[T("1"), T("one")], &[T("2"), T("ok")], &[T("2"), T("two")]],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY l FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "l" found for row"#, detail: "Partition key of the failing row contains (a) = (9).", schema: "public", table: "l", ..E }),
                    copy_from_stdin_file: "partition-routing-bad.tsv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rows inserted into range partitioned tables go to their partitions or the default one",
            set_up_script: &[
                "CREATE TABLE r (a INT, b INT, c TEXT) PARTITION BY RANGE (a, b);",
                "CREATE TABLE r1 PARTITION OF r FOR VALUES FROM (MINVALUE, MINVALUE) TO (10, 5);",
                "CREATE TABLE r2 PARTITION OF r FOR VALUES FROM (10, 5) TO (20, MAXVALUE);",
                "CREATE TABLE rd PARTITION OF r DEFAULT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO r VALUES (1, 1, 'r1'), (10, 4, 'r1'), (10, 5, 'r2'), (19, 100, 'r2'), (20, 0, 'rd'), (NULL, 1, 'rd');",
                    expected: Expected::Tag("INSERT 0 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r1 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[&[T("1"), T("1"), T("r1")], &[T("10"), T("4"), T("r1")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[&[T("10"), T("5"), T("r2")], &[T("19"), T("100"), T("r2")], &[T("20"), T("0"), T("rd")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rd ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[&[Null, T("1"), T("rd")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rd VALUES (5, 5, 'bad');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "rd" violates partition constraint"#, detail: "Failing row contains (5, 5, bad).", schema: "public", table: "rd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r2 VALUES (21, 0, 'bad');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "r2" violates partition constraint"#, detail: "Failing row contains (21, 0, bad).", schema: "public", table: "r2", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rows inserted into hash partitioned tables go to the partitions their hashes pick",
            set_up_script: &[
                "CREATE TABLE h (a INT, b TEXT) PARTITION BY HASH (a);",
                "CREATE TABLE h0 PARTITION OF h FOR VALUES WITH (MODULUS 2, REMAINDER 0);",
                "CREATE TABLE h1 PARTITION OF h FOR VALUES WITH (MODULUS 2, REMAINDER 1);",
                "INSERT INTO h VALUES (1, '1'), (2, '2'), (3, '3'), (4, '4'), (5, '5'), (6, '6'), (7, '7'), (8, '8'), (9, '9'), (10, '10');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a FROM h0 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[&[T("1")], &[T("2")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM h1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[&[T("3")], &[T("4")], &[T("5")], &[T("6")], &[T("7")], &[T("8")], &[T("9")], &[T("10")]],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO h0 VALUES (3, 'x');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "h0" violates partition constraint"#, detail: "Failing row contains (3, x).", schema: "public", table: "h0", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT satisfies_hash_partition('h'::regclass, 2, 0, 1), satisfies_hash_partition('h'::regclass, 2, 1, 1);",
                    expected: Expected::Rows {
                        columns: &[Column("satisfies_hash_partition", BOOL), Column("satisfies_hash_partition", BOOL)],
                        rows: &[&[T("t"), T("f")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT satisfies_hash_partition('h'::regclass, 2, 0, 1, 2);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "number of partitioning columns (1) does not match number of partition keys provided (2)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT satisfies_hash_partition('pg_class'::regclass, 2, 0, 1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#""pg_class" is not a hash partitioned table"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT satisfies_hash_partition('h'::regclass, 0, 0, 1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "modulus for hash partition must be an integer value greater than zero", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rows are routed through partitioned partitions and default partitions",
            set_up_script: &[
                "CREATE TABLE m (a INT, b TEXT, c INT) PARTITION BY LIST (b);",
                "CREATE TABLE m2 PARTITION OF m FOR VALUES IN ('y') PARTITION BY RANGE (c);",
                "CREATE TABLE m21 PARTITION OF m2 FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE ld (a INT, b TEXT) PARTITION BY LIST (a);",
                "CREATE TABLE ld1 PARTITION OF ld FOR VALUES IN (1);",
                "CREATE TABLE ldd PARTITION OF ld DEFAULT PARTITION BY RANGE (a);",
                "CREATE TABLE ldd1 PARTITION OF ldd FOR VALUES FROM (0) TO (10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO m VALUES (1, 'y', 5) RETURNING *, tableoid::regclass::text;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", INT4), Column("tableoid", TEXT)],
                        rows: &[&[T("1"), T("y"), T("5"), T("m21")]],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO m VALUES (1, 'y', 50);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "m2" found for row"#, detail: "Partition key of the failing row contains (c) = (50).", schema: "public", table: "m2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO m VALUES (1, 'z', 5);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "m" found for row"#, detail: "Partition key of the failing row contains (b) = (z).", schema: "public", table: "m", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO m2 VALUES (1, 'z', 5);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "m2" violates partition constraint"#, detail: "Failing row contains (1, z, 5).", schema: "public", table: "m2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ld VALUES (1, 'a'), (5, 'b') RETURNING tableoid::regclass::text, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("ld1"), T("1"), T("a")], &[T("ldd1"), T("5"), T("b")]],
                        tag: "INSERT 0 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ld VALUES (50, 'c');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "ldd" found for row"#, detail: "Partition key of the failing row contains (a) = (50).", schema: "public", table: "ldd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ldd VALUES (1, 'd');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "ldd" violates partition constraint"#, detail: "Failing row contains (1, d).", schema: "public", table: "ldd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ld VALUES (NULL, 'e');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "ldd" found for row"#, detail: "Partition key of the failing row contains (a) = (null).", schema: "public", table: "ldd", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "routed rows are converted to partitions whose columns differ and checked against their constraints",
            set_up_script: &[
                "CREATE TABLE dc (a INT, x INT, b TEXT) PARTITION BY LIST (b);",
                "ALTER TABLE dc DROP COLUMN x;",
                "CREATE TABLE dc1 PARTITION OF dc FOR VALUES IN ('y');",
                "CREATE TABLE dcd PARTITION OF dc DEFAULT;",
                "CREATE TABLE ck (a INT CHECK (a > 0), b TEXT NOT NULL) PARTITION BY RANGE (a);",
                "CREATE TABLE ck1 PARTITION OF ck FOR VALUES FROM (0) TO (100);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dc VALUES (1, 'y'), (2, 'n') RETURNING tableoid::regclass::text, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("dc1"), T("1"), T("y")], &[T("dcd"), T("2"), T("n")]],
                        tag: "INSERT 0 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dc1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("y")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dcd VALUES (3, 'y');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "dcd" violates partition constraint"#, detail: "Failing row contains (3, y).", schema: "public", table: "dcd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dc1 SET b = 'z';",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "dc1" violates partition constraint"#, detail: "Failing row contains (1, z).", schema: "public", table: "dc1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ck VALUES (0, 'x');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "ck1" violates check constraint "ck_a_check""#, detail: "Failing row contains (0, x).", schema: "public", table: "ck1", constraint: "ck_a_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ck VALUES (1, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "b" of relation "ck1" violates not-null constraint"#, detail: "Failing row contains (1, null).", schema: "public", table: "ck1", column: "b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ck SELECT a, b FROM dc1;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ck1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("y")]],
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
fn test_partitioned_and_inherited_table_reads() {
    run_scripts(&[
        ScriptTest {
            name: "reads of partitioned tables append their partitions' rows",
            set_up_script: &[
                "CREATE TABLE n (i INT);",
                "INSERT INTO n VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9);",
                "CREATE TABLE r (a INT, b INT, c TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE r1 PARTITION OF r FOR VALUES FROM (0) TO (500);",
                "CREATE TABLE r2 PARTITION OF r FOR VALUES FROM (500) TO (1000);",
                "INSERT INTO r SELECT x.i * 100 + y.i * 10 + z.i, (x.i * 100 + y.i * 10 + z.i) % 37, 'v' FROM n x, n y, n z;",
                "CREATE INDEX r_b ON r (b);",
                "ANALYZE r;",
                "ANALYZE n;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM r;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("1000"), T("499500")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r ORDER BY b, a LIMIT 5;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("v")],
                            &[T("37"), T("0"), T("v")],
                            &[T("74"), T("0"), T("v")],
                            &[T("111"), T("0"), T("v")],
                            &[T("148"), T("0"), T("v")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM n JOIN r ON r.b = n.i WHERE n.i < 2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[&[T("55")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(*) FROM r GROUP BY b ORDER BY b LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8)],
                        rows: &[&[T("0"), T("28")], &[T("1"), T("27")], &[T("2"), T("27")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "plans of reads of partitioned tables append their partitions' rows",
            set_up_script: &[
                "CREATE TABLE n (i INT);",
                "INSERT INTO n VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9);",
                "CREATE TABLE r (a INT, b INT, c TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE r1 PARTITION OF r FOR VALUES FROM (0) TO (500);",
                "CREATE TABLE r2 PARTITION OF r FOR VALUES FROM (500) TO (1000);",
                "INSERT INTO r SELECT x.i * 100 + y.i * 10 + z.i, (x.i * 100 + y.i * 10 + z.i) % 37, 'v' FROM n x, n y, n z;",
                "CREATE INDEX r_b ON r (b);",
                "ANALYZE r;",
                "ANALYZE n;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r ORDER BY b LIMIT 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Merge Append"#)],
                            &[T(r#"        Sort Key: r.b"#)],
                            &[T(r#"        ->  Index Scan using r1_b_idx on r1 r_1"#)],
                            &[T(r#"        ->  Index Scan using r2_b_idx on r2 r_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT n.i, r.a FROM n JOIN r ON r.b = n.i WHERE n.i < 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  ->  Seq Scan on n"#)],
                            &[T(r#"        Filter: (i < 2)"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Bitmap Heap Scan on r1 r_1"#)],
                            &[T(r#"              Recheck Cond: (b = n.i)"#)],
                            &[T(r#"              ->  Bitmap Index Scan on r1_b_idx"#)],
                            &[T(r#"                    Index Cond: (b = n.i)"#)],
                            &[T(r#"        ->  Bitmap Heap Scan on r2 r_2"#)],
                            &[T(r#"              Recheck Cond: (b = n.i)"#)],
                            &[T(r#"              ->  Bitmap Index Scan on r2_b_idx"#)],
                            &[T(r#"                    Index Cond: (b = n.i)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, count(*) FROM r GROUP BY b ORDER BY b LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  GroupAggregate"#)],
                            &[T(r#"        Group Key: r.b"#)],
                            &[T(r#"        ->  Merge Append"#)],
                            &[T(r#"              Sort Key: r.b"#)],
                            &[T(r#"              ->  Index Only Scan using r1_b_idx on r1 r_1"#)],
                            &[T(r#"              ->  Index Only Scan using r2_b_idx on r2 r_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "reads of partitioned tables go through partitioned partitions and partitions whose columns differ",
            set_up_script: &[
                "CREATE TABLE s (a INT, x INT, b TEXT) PARTITION BY LIST (b);",
                "ALTER TABLE s DROP COLUMN x;",
                "CREATE TABLE s1 PARTITION OF s FOR VALUES IN ('a', 'b') PARTITION BY RANGE (a);",
                "CREATE TABLE s11 PARTITION OF s1 FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE s12 PARTITION OF s1 FOR VALUES FROM (10) TO (20);",
                "CREATE TABLE s2 PARTITION OF s FOR VALUES IN ('c');",
                "INSERT INTO s VALUES (1, 'a'), (11, 'b'), (5, 'c');",
                "CREATE TABLE e (a INT) PARTITION BY LIST (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM s ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("a")], &[T("5"), T("c")], &[T("11"), T("b")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass::text, * FROM s1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("s11"), T("1"), T("a")], &[T("s12"), T("11"), T("b")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM e;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[], tag: "SELECT 0" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "plans of reads of partitioned tables go through partitioned partitions and partitions whose columns differ",
            set_up_script: &[
                "CREATE TABLE s (a INT, x INT, b TEXT) PARTITION BY LIST (b);",
                "ALTER TABLE s DROP COLUMN x;",
                "CREATE TABLE s1 PARTITION OF s FOR VALUES IN ('a', 'b') PARTITION BY RANGE (a);",
                "CREATE TABLE s11 PARTITION OF s1 FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE s12 PARTITION OF s1 FOR VALUES FROM (10) TO (20);",
                "CREATE TABLE s2 PARTITION OF s FOR VALUES IN ('c');",
                "INSERT INTO s VALUES (1, 'a'), (11, 'b'), (5, 'c');",
                "CREATE TABLE e (a INT) PARTITION BY LIST (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF, VERBOSE) SELECT a FROM s WHERE b <> 'z';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on public.s11 s_1"#)],
                            &[T(r#"        Output: s_1.a"#)],
                            &[T(r#"        Filter: (s_1.b <> 'z'::text)"#)],
                            &[T(r#"  ->  Seq Scan on public.s12 s_2"#)],
                            &[T(r#"        Output: s_2.a"#)],
                            &[T(r#"        Filter: (s_2.b <> 'z'::text)"#)],
                            &[T(r#"  ->  Seq Scan on public.s2 s_3"#)],
                            &[T(r#"        Output: s_3.a"#)],
                            &[T(r#"        Filter: (s_3.b <> 'z'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: s.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on s11 s_1"#)],
                            &[T(r#"        ->  Seq Scan on s12 s_2"#)],
                            &[T(r#"        ->  Seq Scan on s2 s_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "reads of inheritance parents append their children's rows",
            set_up_script: &[
                "CREATE TABLE p (x INT, y TEXT);",
                "CREATE TABLE c1 (z INT) INHERITS (p);",
                "INSERT INTO p VALUES (1, 'p');",
                "INSERT INTO c1 VALUES (2, 'c', 9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM p ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[&[T("1"), T("p")], &[T("2"), T("c")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ONLY p;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[&[T("1"), T("p")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "plans of reads of inheritance parents append their children's rows",
            set_up_script: &[
                "CREATE TABLE p (x INT, y TEXT);",
                "CREATE TABLE c1 (z INT) INHERITS (p);",
                "INSERT INTO p VALUES (1, 'p');",
                "INSERT INTO c1 VALUES (2, 'c', 9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on p p_1"#)],
                            &[T(r#"  ->  Seq Scan on c1 p_2"#)],
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
fn test_partitioned_reads_in_partition_order() {
    run_scripts(&[
        ScriptTest {
            name: "partitioned tables read in the order of their bounds",
            set_up_script: &[
                "CREATE TABLE n (i INT);",
                "INSERT INTO n VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9);",
                "CREATE TABLE r (a INT, b INT) PARTITION BY RANGE (a);",
                "CREATE TABLE r1 PARTITION OF r FOR VALUES FROM (0) TO (500);",
                "CREATE TABLE r2 PARTITION OF r FOR VALUES FROM (500) TO (1000);",
                "INSERT INTO r SELECT x.i * 100 + y.i * 10 + z.i, z.i FROM n x, n y, n z;",
                "CREATE INDEX ON r (a);",
                "ANALYZE r;",
                "CREATE TABLE l (a INT, b INT) PARTITION BY LIST (a);",
                "CREATE TABLE l1 PARTITION OF l FOR VALUES IN (1, 2);",
                "CREATE TABLE l2 PARTITION OF l FOR VALUES IN (3, 4);",
                "CREATE TABLE l3 PARTITION OF l FOR VALUES IN (0, 5);",
                "INSERT INTO l SELECT x.i % 6, x.i FROM n x, n y, n z;",
                "CREATE INDEX ON l (a);",
                "ANALYZE l;",
                "CREATE TABLE l4 (a INT, b INT) PARTITION BY LIST (a);",
                "CREATE TABLE l41 PARTITION OF l4 FOR VALUES IN (1, 2);",
                "CREATE TABLE l42 PARTITION OF l4 FOR VALUES IN (3, 4);",
                "INSERT INTO l4 SELECT x.i % 4 + 1, x.i FROM n x, n y, n z;",
                "CREATE INDEX ON l4 (a);",
                "ANALYZE l4;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r ORDER BY a LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Index Scan using r1_a_idx on r1 r_1"#)],
                            &[T(r#"        ->  Index Scan using r2_a_idx on r2 r_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r ORDER BY a LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[&[T("0"), T("0")], &[T("1"), T("1")], &[T("2"), T("2")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r ORDER BY a DESC LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Index Scan Backward using r2_a_idx on r2 r_2"#)],
                            &[T(r#"        ->  Index Scan Backward using r1_a_idx on r1 r_1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r ORDER BY a DESC LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[&[T("999"), T("9")], &[T("998"), T("8")], &[T("997"), T("7")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l ORDER BY a LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Merge Append"#)],
                            &[T(r#"        Sort Key: l.a"#)],
                            &[T(r#"        ->  Index Scan using l3_a_idx on l3 l_1"#)],
                            &[T(r#"        ->  Index Scan using l1_a_idx on l1 l_2"#)],
                            &[T(r#"        ->  Index Scan using l2_a_idx on l2 l_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l4 ORDER BY a LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Index Scan using l41_a_idx on l41 l4_1"#)],
                            &[T(r#"        ->  Index Scan using l42_a_idx on l42 l4_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "partitioned tables read in the order of their bounds under an incremental sort",
            set_up_script: &[
                "CREATE TABLE n (i INT);",
                "INSERT INTO n VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9);",
                "CREATE TABLE r (a INT, b INT) PARTITION BY RANGE (a);",
                "CREATE TABLE r1 PARTITION OF r FOR VALUES FROM (0) TO (500);",
                "CREATE TABLE r2 PARTITION OF r FOR VALUES FROM (500) TO (1000);",
                "INSERT INTO r SELECT x.i * 100 + y.i * 10 + z.i, z.i FROM n x, n y, n z;",
                "CREATE INDEX ON r (a);",
                "ANALYZE r;",
                "CREATE TABLE l (a INT, b INT) PARTITION BY LIST (a);",
                "CREATE TABLE l1 PARTITION OF l FOR VALUES IN (1, 2);",
                "CREATE TABLE l2 PARTITION OF l FOR VALUES IN (3, 4);",
                "CREATE TABLE l3 PARTITION OF l FOR VALUES IN (0, 5);",
                "INSERT INTO l SELECT x.i % 6, x.i FROM n x, n y, n z;",
                "CREATE INDEX ON l (a);",
                "ANALYZE l;",
                "CREATE TABLE l4 (a INT, b INT) PARTITION BY LIST (a);",
                "CREATE TABLE l41 PARTITION OF l4 FOR VALUES IN (1, 2);",
                "CREATE TABLE l42 PARTITION OF l4 FOR VALUES IN (3, 4);",
                "INSERT INTO l4 SELECT x.i % 4 + 1, x.i FROM n x, n y, n z;",
                "CREATE INDEX ON l4 (a);",
                "ANALYZE l4;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r ORDER BY a, b LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Incremental Sort"#)],
                            &[T(r#"        Sort Key: r.a, r.b"#)],
                            &[T(r#"        Presorted Key: r.a"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Index Scan using r1_a_idx on r1 r_1"#)],
                            &[T(r#"              ->  Index Scan using r2_a_idx on r2 r_2"#)],
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
fn test_inheritance_constraint_exclusion() {
    run_scripts(&[
        ScriptTest {
            name: "inheritance children whose constraints refute the conditions are left out",
            set_up_script: &[
                "CREATE TABLE p (a INT, b TEXT);",
                "CREATE TABLE c1 (CHECK (a < 10)) INHERITS (p);",
                "CREATE TABLE c2 (CHECK (a >= 10 AND a < 20)) INHERITS (p);",
                "CREATE TABLE c3 (CHECK (a IN (30, 31, 32))) INHERITS (p);",
                "INSERT INTO c1 VALUES (1, 'x');",
                "INSERT INTO c2 VALUES (15, 'y');",
                "INSERT INTO c3 VALUES (31, 'z');",
                "CREATE TABLE t (a INT CHECK (a > 0));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM p WHERE a = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on p p_1"#)],
                            &[T(r#"        Filter: (a = 15)"#)],
                            &[T(r#"  ->  Seq Scan on c2 p_2"#)],
                            &[T(r#"        Filter: (a = 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p WHERE a = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("15"), T("y")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM p WHERE a < 5 OR a = 31;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on p p_1"#)],
                            &[T(r#"        Filter: ((a < 5) OR (a = 31))"#)],
                            &[T(r#"  ->  Seq Scan on c1 p_2"#)],
                            &[T(r#"        Filter: ((a < 5) OR (a = 31))"#)],
                            &[T(r#"  ->  Seq Scan on c3 p_3"#)],
                            &[T(r#"        Filter: ((a < 5) OR (a = 31))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM p WHERE a IN (1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on p p_1"#)],
                            &[T(r#"        Filter: (a = ANY ('{1,2}'::integer[]))"#)],
                            &[T(r#"  ->  Seq Scan on c1 p_2"#)],
                            &[T(r#"        Filter: (a = ANY ('{1,2}'::integer[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM p WHERE a = 1 AND a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[&[T(r#"Result"#)], &[T(r#"  One-Time Filter: false"#)]],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET constraint_exclusion = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t WHERE a < 0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[&[T(r#"Result"#)], &[T(r#"  One-Time Filter: false"#)]],
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
fn test_inheritance_row_marks() {
    run_scripts(&[
        ScriptTest {
            name: "FOR UPDATE and FOR SHARE lock the rows of inheritance and partitioned tables",
            set_up_script: &[
                "CREATE TABLE p (a INT, b TEXT);",
                "CREATE TABLE c1 (CHECK (a < 10)) INHERITS (p);",
                "CREATE TABLE c2 (CHECK (a >= 10)) INHERITS (p);",
                "INSERT INTO p VALUES (0, 'p');",
                "INSERT INTO c1 VALUES (1, 'x');",
                "INSERT INTO c2 VALUES (15, 'y');",
                "CREATE TABLE q (a INT, z TEXT);",
                "INSERT INTO q VALUES (1, 'q1'), (15, 'q15');",
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (100) PARTITION BY LIST (b);",
                "CREATE TABLE pt2a PARTITION OF pt2 FOR VALUES IN ('a');",
                "CREATE TABLE pt2b PARTITION OF pt2 FOR VALUES IN ('b');",
                "INSERT INTO pt VALUES (1, 'a'), (11, 'a'), (12, 'b');",
                "CREATE TABLE ep (a INT) PARTITION BY LIST (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM p FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on p p_1"#)],
                            &[T(r#"        ->  Seq Scan on c1 p_2"#)],
                            &[T(r#"        ->  Seq Scan on c2 p_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p ORDER BY a FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("0"), T("p")],
                            &[T("1"), T("x")],
                            &[T("15"), T("y")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p WHERE a > 0 ORDER BY a FOR SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("c1"), T("1"), T("x")],
                            &[T("c2"), T("15"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p WHERE a = 15 FOR KEY SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("15"), T("y")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ONLY p FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("0"), T("p")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p JOIN q USING (a) ORDER BY a FOR UPDATE OF p;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("z", TEXT)],
                        rows: &[
                            &[T("1"), T("x"), T("q1")],
                            &[T("15"), T("y"), T("q15")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q JOIN p USING (a) ORDER BY a FOR UPDATE OF q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("z", TEXT), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("q1"), T("x")],
                            &[T("15"), T("q15"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM pt FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on pt1 pt_1"#)],
                            &[T(r#"        ->  Seq Scan on pt2a pt_2"#)],
                            &[T(r#"        ->  Seq Scan on pt2b pt_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt ORDER BY a FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt1"), T("1"), T("a")],
                            &[T("pt2a"), T("11"), T("a")],
                            &[T("pt2b"), T("12"), T("b")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt JOIN p USING (a) ORDER BY a FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("b", TEXT)],
                        rows: &[&[T("1"), T("a"), T("x")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt LEFT JOIN p ON pt.a = p.a + 100 ORDER BY pt.a FOR UPDATE OF pt;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a"), Null, Null],
                            &[T("11"), T("a"), Null, Null],
                            &[T("12"), T("b"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT * FROM pt WHERE a < 5) s FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("a")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE q SET z = z || '!' FROM p WHERE p.a = q.a;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM q USING pt WHERE pt.a = q.a AND pt.a = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("z", TEXT)],
                        rows: &[&[T("15"), T("q15!")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ep FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM ep FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "locking the rows of inheritance children waits for, skips, or rechecks rows other transactions hold",
            set_up_script: &[
                "CREATE TABLE p (a INT, b TEXT);",
                "CREATE TABLE c1 (PRIMARY KEY (a), CHECK (a < 10)) INHERITS (p);",
                "CREATE TABLE c2 (PRIMARY KEY (a), CHECK (a >= 10)) INHERITS (p);",
                "INSERT INTO p VALUES (0, 'p');",
                "INSERT INTO c1 VALUES (1, 'x');",
                "INSERT INTO c2 VALUES (15, 'y');",
                "CREATE TABLE q (a INT PRIMARY KEY, z TEXT);",
                "INSERT INTO q VALUES (1, 'q1'), (15, 'q15');",
                "CREATE TABLE pt (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt (PRIMARY KEY (a)) FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt (PRIMARY KEY (a)) FOR VALUES FROM (10) TO (100);",
                "INSERT INTO pt VALUES (1, 'a'), (11, 'a'), (12, 'b');",
                "CREATE TABLE seen (s TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM pt WHERE a = 11 FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("11"), T("a")]],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM pt ORDER BY a FOR UPDATE SKIP LOCKED;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("12"), T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM pt ORDER BY a FOR SHARE NOWAIT;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: r#"could not obtain lock on row in relation "pt2""#, ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE pt2 SET b = 'c' WHERE a = 11;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT b FROM pt WHERE a > 5 FOR UPDATE;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM seen ORDER BY s;",
                    expected: Expected::Rows {
                        columns: &[Column("s", TEXT)],
                        rows: &[
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE c1 SET a = 2 WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT 'gone ' || b FROM p WHERE a = 1 FOR UPDATE;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE q SET z = 'q15b' WHERE a = 15;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT z || ' ' || p.b FROM q JOIN p USING (a) WHERE q.a > 5 FOR UPDATE OF q;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM seen ORDER BY s;",
                    expected: Expected::Rows {
                        columns: &[Column("s", TEXT)],
                        rows: &[
                            &[T("b")],
                            &[T("c")],
                            &[T("q15b y")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an Append whose rows a projection computes on runs under a Result",
            set_up_script: &[
                "CREATE TABLE p (a INT, b TEXT);",
                "CREATE TABLE c1 (CHECK (a < 10)) INHERITS (p);",
                "INSERT INTO p VALUES (0, 'p');",
                "INSERT INTO c1 VALUES (1, 'x');",
                "CREATE TABLE q (a INT, b TEXT);",
                "INSERT INTO q VALUES (100, 'q'), (101, 'r');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM q JOIN p ON q.a = p.a + 100;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Merge Join"#)],
                            &[T(r#"  Merge Cond: (q.a = ((p.a + 100)))"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: q.a"#)],
                            &[T(r#"        ->  Seq Scan on q"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: ((p.a + 100))"#)],
                            &[T(r#"        ->  Result"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Seq Scan on p p_1"#)],
                            &[T(r#"                    ->  Seq Scan on c1 p_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q JOIN p ON q.a = p.a + 100 ORDER BY q.a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("100"), T("q"), T("0"), T("p")],
                            &[T("101"), T("r"), T("1"), T("x")],
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
fn test_plan_time_partition_pruning() {
    run_scripts(&[
        ScriptTest {
            name: "partitions the conditions rule out are left out of plans",
            set_up_script: &[
                "CREATE TABLE r (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE r1 PARTITION OF r FOR VALUES FROM (MINVALUE) TO (10);",
                "CREATE TABLE r2 PARTITION OF r FOR VALUES FROM (10) TO (20);",
                "CREATE TABLE r3 PARTITION OF r FOR VALUES FROM (20) TO (30);",
                "CREATE TABLE rd PARTITION OF r DEFAULT;",
                "INSERT INTO r VALUES (1, 'a'), (15, 'b'), (25, 'c'), (35, 'd'), (NULL, 'n');",
                "CREATE TABLE l (a INT, b TEXT) PARTITION BY LIST (a);",
                "CREATE TABLE l1 PARTITION OF l FOR VALUES IN (1, 2);",
                "CREATE TABLE l2 PARTITION OF l FOR VALUES IN (3, 4);",
                "CREATE TABLE ln PARTITION OF l FOR VALUES IN (NULL, 5);",
                "CREATE TABLE ld PARTITION OF l DEFAULT;",
                "INSERT INTO l VALUES (1, 'a'), (3, 'b'), (5, 'c'), (NULL, 'n'), (9, 'd');",
                "CREATE TABLE h (a INT, b TEXT) PARTITION BY HASH (a);",
                "CREATE TABLE h0 PARTITION OF h FOR VALUES WITH (MODULUS 3, REMAINDER 0);",
                "CREATE TABLE h1 PARTITION OF h FOR VALUES WITH (MODULUS 3, REMAINDER 1);",
                "CREATE TABLE h2 PARTITION OF h FOR VALUES WITH (MODULUS 3, REMAINDER 2);",
                "INSERT INTO h VALUES (1, '1'), (2, '2'), (3, '3'), (4, '4'), (5, '5'), (6, '6'), (7, '7'), (8, '8'), (9, '9');",
                "CREATE TABLE m (a INT, b INT, c TEXT) PARTITION BY RANGE (a, b);",
                "CREATE TABLE m1 PARTITION OF m FOR VALUES FROM (0, 0) TO (10, 10);",
                "CREATE TABLE m2 PARTITION OF m FOR VALUES FROM (10, 10) TO (10, 20);",
                "CREATE TABLE m3 PARTITION OF m FOR VALUES FROM (10, 20) TO (20, 0);",
                "CREATE TABLE m4 PARTITION OF m FOR VALUES FROM (20, 0) TO (MAXVALUE, MAXVALUE);",
                "CREATE TABLE mh (a INT, b TEXT) PARTITION BY HASH (a, b);",
                "CREATE TABLE mh0 PARTITION OF mh FOR VALUES WITH (MODULUS 2, REMAINDER 0);",
                "CREATE TABLE mh1 PARTITION OF mh FOR VALUES WITH (MODULUS 2, REMAINDER 1);",
                "CREATE TABLE bp (a BOOL, b INT) PARTITION BY LIST (a);",
                "CREATE TABLE bpt PARTITION OF bp FOR VALUES IN (TRUE);",
                "CREATE TABLE bpf PARTITION OF bp FOR VALUES IN (FALSE);",
                "CREATE TABLE bpn PARTITION OF bp FOR VALUES IN (NULL);",
                "CREATE TABLE s (a INT, b INT) PARTITION BY RANGE (a);",
                "CREATE TABLE s1 PARTITION OF s FOR VALUES FROM (0) TO (100) PARTITION BY RANGE (a);",
                "CREATE TABLE s1a PARTITION OF s1 FOR VALUES FROM (0) TO (50);",
                "CREATE TABLE s1d PARTITION OF s1 DEFAULT;",
                "CREATE TABLE s2 PARTITION OF s FOR VALUES FROM (100) TO (200) PARTITION BY LIST (b);",
                "CREATE TABLE s2a PARTITION OF s2 FOR VALUES IN (1);",
                "CREATE TABLE s2b PARTITION OF s2 FOR VALUES IN (2);",
                "CREATE TABLE tx (a TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE tx1 PARTITION OF tx FOR VALUES FROM ('a') TO ('m');",
                "CREATE TABLE tx2 PARTITION OF tx FOR VALUES FROM ('m') TO ('z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on r2 r"#)],
                            &[T(r#"  Filter: (a = 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a < 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: (a < 15)"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: (a < 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a <= 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: (a <= 10)"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: (a <= 10)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a > 25;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on r3 r_1"#)],
                            &[T(r#"        Filter: (a > 25)"#)],
                            &[T(r#"  ->  Seq Scan on rd r_2"#)],
                            &[T(r#"        Filter: (a > 25)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a >= 20 AND a < 30;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on r3 r"#)],
                            &[T(r#"  Filter: ((a >= 20) AND (a < 30))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = 1 OR a = 25;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: ((a = 1) OR (a = 25))"#)],
                            &[T(r#"  ->  Seq Scan on r3 r_2"#)],
                            &[T(r#"        Filter: ((a = 1) OR (a = 25))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a IN (1, 15);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: (a = ANY ('{1,15}'::integer[]))"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: (a = ANY ('{1,15}'::integer[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on rd r"#)],
                            &[T(r#"  Filter: (a IS NULL)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: (a IS NOT NULL)"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: (a IS NOT NULL)"#)],
                            &[T(r#"  ->  Seq Scan on r3 r_3"#)],
                            &[T(r#"        Filter: (a IS NOT NULL)"#)],
                            &[T(r#"  ->  Seq Scan on rd r_4"#)],
                            &[T(r#"        Filter: (a IS NOT NULL)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = 1 AND a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = 15::BIGINT;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on r2 r"#)],
                            &[T(r#"  Filter: (a = '15'::bigint)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = 100;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on rd r"#)],
                            &[T(r#"  Filter: (a = 100)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE 15 = a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on r2 r"#)],
                            &[T(r#"  Filter: (15 = a)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = 15 AND b = 'b';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on r2 r"#)],
                            &[T(r#"  Filter: ((a = 15) AND (b = 'b'::text))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE b = 'b';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: (b = 'b'::text)"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: (b = 'b'::text)"#)],
                            &[T(r#"  ->  Seq Scan on r3 r_3"#)],
                            &[T(r#"        Filter: (b = 'b'::text)"#)],
                            &[T(r#"  ->  Seq Scan on rd r_4"#)],
                            &[T(r#"        Filter: (b = 'b'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = ANY (ARRAY[1, NULL]);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on r1 r"#)],
                            &[T(r#"  Filter: (a = ANY ('{1,NULL}'::integer[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = ALL (ARRAY[1, 2]);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a IN (NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r WHERE a < 15 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r WHERE a > 25 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("35"), T("d")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_partition_pruning = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: (a = 15)"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: (a = 15)"#)],
                            &[T(r#"  ->  Seq Scan on r3 r_3"#)],
                            &[T(r#"        Filter: (a = 15)"#)],
                            &[T(r#"  ->  Seq Scan on rd r_4"#)],
                            &[T(r#"        Filter: (a = 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_partition_pruning;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on l2 l"#)],
                            &[T(r#"  Filter: (a = 3)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a <> 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on l1 l_1"#)],
                            &[T(r#"        Filter: (a <> 3)"#)],
                            &[T(r#"  ->  Seq Scan on l2 l_2"#)],
                            &[T(r#"        Filter: (a <> 3)"#)],
                            &[T(r#"  ->  Seq Scan on ln l_3"#)],
                            &[T(r#"        Filter: (a <> 3)"#)],
                            &[T(r#"  ->  Seq Scan on ld l_4"#)],
                            &[T(r#"        Filter: (a <> 3)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a NOT IN (1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on l2 l_1"#)],
                            &[T(r#"        Filter: (a <> ALL ('{1,2}'::integer[]))"#)],
                            &[T(r#"  ->  Seq Scan on ln l_2"#)],
                            &[T(r#"        Filter: (a <> ALL ('{1,2}'::integer[]))"#)],
                            &[T(r#"  ->  Seq Scan on ld l_3"#)],
                            &[T(r#"        Filter: (a <> ALL ('{1,2}'::integer[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on ln l"#)],
                            &[T(r#"  Filter: (a IS NULL)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on l1 l_1"#)],
                            &[T(r#"        Filter: (a IS NOT NULL)"#)],
                            &[T(r#"  ->  Seq Scan on l2 l_2"#)],
                            &[T(r#"        Filter: (a IS NOT NULL)"#)],
                            &[T(r#"  ->  Seq Scan on ln l_3"#)],
                            &[T(r#"        Filter: (a IS NOT NULL)"#)],
                            &[T(r#"  ->  Seq Scan on ld l_4"#)],
                            &[T(r#"        Filter: (a IS NOT NULL)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on l1 l_1"#)],
                            &[T(r#"        Filter: (a < 3)"#)],
                            &[T(r#"  ->  Seq Scan on ld l_2"#)],
                            &[T(r#"        Filter: (a < 3)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a > 4;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on ln l_1"#)],
                            &[T(r#"        Filter: (a > 4)"#)],
                            &[T(r#"  ->  Seq Scan on ld l_2"#)],
                            &[T(r#"        Filter: (a > 4)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on ld l"#)],
                            &[T(r#"  Filter: (a = 7)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a IN (1, 3, NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on l1 l_1"#)],
                            &[T(r#"        Filter: (a = ANY ('{1,3,NULL}'::integer[]))"#)],
                            &[T(r#"  ->  Seq Scan on l2 l_2"#)],
                            &[T(r#"        Filter: (a = ANY ('{1,3,NULL}'::integer[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM l WHERE a <> 3 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("5"), T("c")],
                            &[T("9"), T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM l WHERE a IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[Null, T("n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM h WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on h2 h"#)],
                            &[T(r#"  Filter: (a = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM h WHERE a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on h0 h"#)],
                            &[T(r#"  Filter: (a = 2)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM h WHERE a IN (1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on h0 h_1"#)],
                            &[T(r#"        Filter: (a = ANY ('{1,2}'::integer[]))"#)],
                            &[T(r#"  ->  Seq Scan on h2 h_2"#)],
                            &[T(r#"        Filter: (a = ANY ('{1,2}'::integer[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM h WHERE a IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on h0 h"#)],
                            &[T(r#"  Filter: (a IS NULL)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM h WHERE a < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on h0 h_1"#)],
                            &[T(r#"        Filter: (a < 3)"#)],
                            &[T(r#"  ->  Seq Scan on h1 h_2"#)],
                            &[T(r#"        Filter: (a < 3)"#)],
                            &[T(r#"  ->  Seq Scan on h2 h_3"#)],
                            &[T(r#"        Filter: (a < 3)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM h WHERE a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM m WHERE a = 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on m1 m_1"#)],
                            &[T(r#"        Filter: (a = 10)"#)],
                            &[T(r#"  ->  Seq Scan on m2 m_2"#)],
                            &[T(r#"        Filter: (a = 10)"#)],
                            &[T(r#"  ->  Seq Scan on m3 m_3"#)],
                            &[T(r#"        Filter: (a = 10)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM m WHERE a = 10 AND b = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on m2 m"#)],
                            &[T(r#"  Filter: ((a = 10) AND (b = 15))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM m WHERE a = 10 AND b < 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on m1 m_1"#)],
                            &[T(r#"        Filter: ((b < 15) AND (a = 10))"#)],
                            &[T(r#"  ->  Seq Scan on m2 m_2"#)],
                            &[T(r#"        Filter: ((b < 15) AND (a = 10))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM m WHERE a < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on m1 m"#)],
                            &[T(r#"  Filter: (a < 10)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM m WHERE a <= 10 AND b <= 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on m1 m_1"#)],
                            &[T(r#"        Filter: ((a <= 10) AND (b <= 10))"#)],
                            &[T(r#"  ->  Seq Scan on m2 m_2"#)],
                            &[T(r#"        Filter: ((a <= 10) AND (b <= 10))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM m WHERE a > 10 AND b > 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on m3 m_1"#)],
                            &[T(r#"        Filter: ((a > 10) AND (b > 5))"#)],
                            &[T(r#"  ->  Seq Scan on m4 m_2"#)],
                            &[T(r#"        Filter: ((a > 10) AND (b > 5))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM m WHERE b = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on m1 m_1"#)],
                            &[T(r#"        Filter: (b = 15)"#)],
                            &[T(r#"  ->  Seq Scan on m2 m_2"#)],
                            &[T(r#"        Filter: (b = 15)"#)],
                            &[T(r#"  ->  Seq Scan on m3 m_3"#)],
                            &[T(r#"        Filter: (b = 15)"#)],
                            &[T(r#"  ->  Seq Scan on m4 m_4"#)],
                            &[T(r#"        Filter: (b = 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM m WHERE a >= 20;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on m3 m_1"#)],
                            &[T(r#"        Filter: (a >= 20)"#)],
                            &[T(r#"  ->  Seq Scan on m4 m_2"#)],
                            &[T(r#"        Filter: (a >= 20)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM mh WHERE a = 1 AND b = 'x';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on mh0 mh"#)],
                            &[T(r#"  Filter: ((a = 1) AND (b = 'x'::text))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM mh WHERE a = 1 AND b IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on mh0 mh"#)],
                            &[T(r#"  Filter: ((b IS NULL) AND (a = 1))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM mh WHERE a IS NULL AND b IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on mh0 mh"#)],
                            &[T(r#"  Filter: ((a IS NULL) AND (b IS NULL))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM mh WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on mh0 mh_1"#)],
                            &[T(r#"        Filter: (a = 1)"#)],
                            &[T(r#"  ->  Seq Scan on mh1 mh_2"#)],
                            &[T(r#"        Filter: (a = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bp WHERE a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on bpt bp"#)],
                            &[T(r#"  Filter: a"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bp WHERE NOT a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on bpf bp"#)],
                            &[T(r#"  Filter: (NOT a)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bp WHERE a IS TRUE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on bpt bp"#)],
                            &[T(r#"  Filter: (a IS TRUE)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bp WHERE a IS NOT TRUE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on bpf bp_1"#)],
                            &[T(r#"        Filter: (a IS NOT TRUE)"#)],
                            &[T(r#"  ->  Seq Scan on bpn bp_2"#)],
                            &[T(r#"        Filter: (a IS NOT TRUE)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bp WHERE a IS NOT FALSE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on bpt bp_1"#)],
                            &[T(r#"        Filter: (a IS NOT FALSE)"#)],
                            &[T(r#"  ->  Seq Scan on bpn bp_2"#)],
                            &[T(r#"        Filter: (a IS NOT FALSE)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bp WHERE a IS UNKNOWN;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on bpn bp"#)],
                            &[T(r#"  Filter: (a IS UNKNOWN)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bp WHERE a IS NOT UNKNOWN;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on bpf bp_1"#)],
                            &[T(r#"        Filter: (a IS NOT UNKNOWN)"#)],
                            &[T(r#"  ->  Seq Scan on bpt bp_2"#)],
                            &[T(r#"        Filter: (a IS NOT UNKNOWN)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bp WHERE a = FALSE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on bpf bp"#)],
                            &[T(r#"  Filter: (NOT a)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s WHERE a = 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on s1a s"#)],
                            &[T(r#"  Filter: (a = 10)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s WHERE a = 60;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on s1d s"#)],
                            &[T(r#"  Filter: (a = 60)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s WHERE a = 150;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on s2a s_1"#)],
                            &[T(r#"        Filter: (a = 150)"#)],
                            &[T(r#"  ->  Seq Scan on s2b s_2"#)],
                            &[T(r#"        Filter: (a = 150)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s WHERE a = 150 AND b = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on s2b s"#)],
                            &[T(r#"  Filter: ((a = 150) AND (b = 2))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s WHERE a > 120;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on s2a s_1"#)],
                            &[T(r#"        Filter: (a > 120)"#)],
                            &[T(r#"  ->  Seq Scan on s2b s_2"#)],
                            &[T(r#"        Filter: (a > 120)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s WHERE b = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on s1a s_1"#)],
                            &[T(r#"        Filter: (b = 1)"#)],
                            &[T(r#"  ->  Seq Scan on s1d s_2"#)],
                            &[T(r#"        Filter: (b = 1)"#)],
                            &[T(r#"  ->  Seq Scan on s2a s_3"#)],
                            &[T(r#"        Filter: (b = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s1 WHERE a = 150;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Result"#)],
                            &[T(r#"  One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM s WHERE a > 50 AND a < 100;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on s1d s"#)],
                            &[T(r#"  Filter: ((a > 50) AND (a < 100))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM tx WHERE a = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on tx1 tx"#)],
                            &[T(r#"  Filter: (a = 'c'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM tx WHERE a > 'p';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on tx2 tx"#)],
                            &[T(r#"  Filter: (a > 'p'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXPLAIN (COSTS OFF) SELECT * FROM tx WHERE a = 'c' COLLATE "C";"#,
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  Seq Scan on tx1 tx_1"#)],
                            &[T(r#"        Filter: (a = 'c'::text COLLATE "C")"#)],
                            &[T(r#"  ->  Seq Scan on tx2 tx_2"#)],
                            &[T(r#"        Filter: (a = 'c'::text COLLATE "C")"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r JOIN l ON r.a = l.a WHERE r.a = 1 AND l.a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  ->  Seq Scan on r1 r"#)],
                            &[T(r#"        Filter: (a = 1)"#)],
                            &[T(r#"  ->  Materialize"#)],
                            &[T(r#"        ->  Seq Scan on l1 l"#)],
                            &[T(r#"              Filter: (a = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE r SET b = 'x' WHERE a = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on r"#)],
                            &[T(r#"  Update on r2 r_1"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_1"#)],
                            &[T(r#"        Filter: (a = 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = 15 FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  ->  Seq Scan on r2 r"#)],
                            &[T(r#"        Filter: (a = 15)"#)],
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
fn test_run_time_partition_pruning() {
    run_scripts(&[
        ScriptTest {
            name: "run-time pruning leaves out the partitions the conditions rule out once values are known",
            set_up_script: &[
                "CREATE TABLE r (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE r1 PARTITION OF r FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE r2 PARTITION OF r FOR VALUES FROM (10) TO (20);",
                "CREATE TABLE r3 PARTITION OF r FOR VALUES FROM (20) TO (30) PARTITION BY LIST (b);",
                "CREATE TABLE r3a PARTITION OF r3 FOR VALUES IN ('a');",
                "CREATE TABLE r3b PARTITION OF r3 FOR VALUES IN ('b');",
                "INSERT INTO r VALUES (1, 'a'), (15, 'b'), (25, 'a'), (26, 'b');",
                "CREATE TABLE q (a INT);",
                "INSERT INTO q VALUES (1), (25);",
                "CREATE TABLE l (a INT, b INT) PARTITION BY LIST (a);",
                "CREATE TABLE l1 PARTITION OF l FOR VALUES IN (1);",
                "CREATE TABLE l2 PARTITION OF l FOR VALUES IN (2);",
                "CREATE INDEX ON l (b);",
                "INSERT INTO l VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = to_char(15, '99')::INT;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  Subplans Removed: 3"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_1"#)],
                            &[T(r#"        Filter: (a = (to_char(15, '99'::text))::integer)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r WHERE a = to_char(15, '99')::INT;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("15"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = (SELECT 25);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Result"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r3a r_3"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r3b r_4"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM r WHERE a = (SELECT 25);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Result (actual rows=1.00 loops=1)"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1 (never executed)"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2 (never executed)"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r3a r_3 (actual rows=1.00 loops=1)"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r3b r_4 (actual rows=0.00 loops=1)"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"        Rows Removed by Filter: 1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r WHERE a = (SELECT 25) ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("25"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a > (SELECT 5) AND b = to_char(1, 'FM9')::TEXT;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  Subplans Removed: 2"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Result"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: ((a > (InitPlan 1).col1) AND (b = to_char(1, 'FM9'::text)))"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: ((a > (InitPlan 1).col1) AND (b = to_char(1, 'FM9'::text)))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p(INT) AS SELECT * FROM r WHERE a = $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p(15);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on r2 r"#)],
                            &[T(r#"  Filter: (a = 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plan_cache_mode = force_generic_plan;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p(15);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  Subplans Removed: 3"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_1"#)],
                            &[T(r#"        Filter: (a = $1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p(15);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("15"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p(100);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  Subplans Removed: 4"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p2(INT, TEXT) AS SELECT * FROM r WHERE a = $1 AND b = $2;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p2(25, 'b');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  Subplans Removed: 3"#)],
                            &[T(r#"  ->  Seq Scan on r3b r_1"#)],
                            &[T(r#"        Filter: ((a = $1) AND (b = $2))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p2(25, 'b');",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET plan_cache_mode;",
                    expected: Expected::Tag("RESET"),
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
                    query: "SET enable_material = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT q.a, (SELECT count(*) FROM r WHERE r.a = q.a) FROM q;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on q"#)],
                            &[T(r#"  SubPlan 1"#)],
                            &[T(r#"    ->  Aggregate"#)],
                            &[T(r#"          ->  Append"#)],
                            &[T(r#"                ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"                      Filter: (a = q.a)"#)],
                            &[T(r#"                ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"                      Filter: (a = q.a)"#)],
                            &[T(r#"                ->  Seq Scan on r3a r_3"#)],
                            &[T(r#"                      Filter: (a = q.a)"#)],
                            &[T(r#"                ->  Seq Scan on r3b r_4"#)],
                            &[T(r#"                      Filter: (a = q.a)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT q.a, (SELECT count(*) FROM r WHERE r.a = q.a) FROM q;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on q (actual rows=2.00 loops=1)"#)],
                            &[T(r#"  SubPlan 1"#)],
                            &[T(r#"    ->  Aggregate (actual rows=1.00 loops=2)"#)],
                            &[T(r#"          ->  Append (actual rows=1.00 loops=2)"#)],
                            &[T(r#"                ->  Seq Scan on r1 r_1 (actual rows=1.00 loops=1)"#)],
                            &[T(r#"                      Filter: (a = q.a)"#)],
                            &[T(r#"                ->  Seq Scan on r2 r_2 (never executed)"#)],
                            &[T(r#"                      Filter: (a = q.a)"#)],
                            &[T(r#"                ->  Seq Scan on r3a r_3 (actual rows=1.00 loops=1)"#)],
                            &[T(r#"                      Filter: (a = q.a)"#)],
                            &[T(r#"                ->  Seq Scan on r3b r_4 (actual rows=0.00 loops=1)"#)],
                            &[T(r#"                      Filter: (a = q.a)"#)],
                            &[T(r#"                      Rows Removed by Filter: 1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT q.a, (SELECT count(*) FROM r WHERE r.a = q.a) FROM q ORDER BY q.a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("25"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM q WHERE EXISTS (SELECT 1 FROM r WHERE r.a = q.a);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  Join Filter: (q.a = r.a)"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: r.a"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"              ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"              ->  Seq Scan on r3a r_3"#)],
                            &[T(r#"              ->  Seq Scan on r3b r_4"#)],
                            &[T(r#"  ->  Seq Scan on q"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q WHERE EXISTS (SELECT 1 FROM r WHERE r.a = q.a) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("25")],
                        ],
                        tag: "SELECT 2",
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
                    query: "RESET enable_material;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_partition_pruning = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM r WHERE a = (SELECT 25);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Result"#)],
                            &[T(r#"  ->  Seq Scan on r1 r_1"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r2 r_2"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r3a r_3"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"  ->  Seq Scan on r3b r_4"#)],
                            &[T(r#"        Filter: (a = (InitPlan 1).col1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_partition_pruning;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a = (SELECT 2) ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: l.b"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Result"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on l1 l_1"#)],
                            &[T(r#"              Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"        ->  Seq Scan on l2 l_2"#)],
                            &[T(r#"              Filter: (a = (InitPlan 1).col1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM l WHERE a = (SELECT 2) ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM l WHERE a IN (1, (SELECT 2));",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Result"#)],
                            &[T(r#"  ->  Seq Scan on l1 l_1"#)],
                            &[T(r#"        Filter: (a = ANY (ARRAY[1, (InitPlan 1).col1]))"#)],
                            &[T(r#"  ->  Seq Scan on l2 l_2"#)],
                            &[T(r#"        Filter: (a = ANY (ARRAY[1, (InitPlan 1).col1]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rechecks of rows other transactions changed read partitioned tables through their run-time pruning",
            set_up_script: &[
                "CREATE TABLE r (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE r1 PARTITION OF r (PRIMARY KEY (a)) FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE r2 PARTITION OF r (PRIMARY KEY (a)) FOR VALUES FROM (10) TO (20);",
                "INSERT INTO r VALUES (1, 'a'), (15, 'b');",
                "CREATE TABLE q (a INT PRIMARY KEY, z TEXT);",
                "INSERT INTO q VALUES (1, 'q1'), (15, 'q15');",
                "CREATE TABLE seen (s TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE q SET z = 'zz' WHERE a = 15;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE q SET z = z || r.b FROM r WHERE r.a = q.a AND r.a = to_char(15, '99')::INT;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM q ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("z", TEXT)],
                        rows: &[
                            &[T("1"), T("q1")],
                            &[T("15"), T("zzb")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE r2 SET b = 'c' WHERE a = 15;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT b FROM r WHERE a = to_char(15, '99')::INT FOR UPDATE;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM seen;",
                    expected: Expected::Rows {
                        columns: &[Column("s", TEXT)],
                        rows: &[&[T("c")]],
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
fn test_update_and_delete_through_parents() {
    run_scripts(&[
        ScriptTest {
            name: "UPDATE and DELETE change the rows of inheritance and partitioned tables through their parents",
            set_up_script: &[
                "CREATE TABLE p (a INT, b TEXT);",
                "CREATE TABLE c1 (CHECK (a < 10)) INHERITS (p);",
                "CREATE TABLE c2 (x INT, CHECK (a >= 10)) INHERITS (p);",
                "INSERT INTO p VALUES (0, 'p');",
                "INSERT INTO c1 VALUES (1, 'x');",
                "INSERT INTO c2 VALUES (15, 'y', 7);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE p SET b = b || '!';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on p"#)],
                            &[T(r#"  Update on p p_1"#)],
                            &[T(r#"  Update on c1 p_2"#)],
                            &[T(r#"  Update on c2 p_3"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on p p_1"#)],
                            &[T(r#"              ->  Seq Scan on c1 p_2"#)],
                            &[T(r#"              ->  Seq Scan on c2 p_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = b || '!';",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("p"), T("0"), T("p!")],
                            &[T("c1"), T("1"), T("x!")],
                            &[T("c2"), T("15"), T("y!")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE p SET b = 'z' WHERE a = 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on p"#)],
                            &[T(r#"  Update on p p_1"#)],
                            &[T(r#"  Update on c2 p_2"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on p p_1"#)],
                            &[T(r#"                    Filter: (a = 15)"#)],
                            &[T(r#"              ->  Seq Scan on c2 p_2"#)],
                            &[T(r#"                    Filter: (a = 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = 'z' WHERE a = 15 RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("c2"), T("15"), T("z")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ONLY p SET b = 'only' RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("0"), T("only")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM p WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Delete on p"#)],
                            &[T(r#"  Delete on p p_1"#)],
                            &[T(r#"  Delete on c1 p_2"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on p p_1"#)],
                            &[T(r#"              Filter: (a = 1)"#)],
                            &[T(r#"        ->  Seq Scan on c1 p_2"#)],
                            &[T(r#"              Filter: (a = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM p WHERE a = 1 RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("c1"), T("1"), T("x!")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("p"), T("0"), T("only")],
                            &[T("c2"), T("15"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) UPDATE p SET a = a + 1 WHERE b <> 'x';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on public.p"#)],
                            &[T(r#"  Update on public.p p_1"#)],
                            &[T(r#"  Update on public.c1 p_2"#)],
                            &[T(r#"  Update on public.c2 p_3"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        Output: (p.a + 1), p.tableoid, p.ctid"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on public.p p_1"#)],
                            &[T(r#"                    Output: p_1.a, p_1.tableoid, p_1.ctid"#)],
                            &[T(r#"                    Filter: (p_1.b <> 'x'::text)"#)],
                            &[T(r#"              ->  Seq Scan on public.c1 p_2"#)],
                            &[T(r#"                    Output: p_2.a, p_2.tableoid, p_2.ctid"#)],
                            &[T(r#"                    Filter: (p_2.b <> 'x'::text)"#)],
                            &[T(r#"              ->  Seq Scan on public.c2 p_3"#)],
                            &[T(r#"                    Output: p_3.a, p_3.tableoid, p_3.ctid"#)],
                            &[T(r#"                    Filter: (p_3.b <> 'x'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET a = a + 1 WHERE b <> 'x';",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("p"), T("1"), T("only")],
                            &[T("c2"), T("16"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET a = 100 WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET a = 5 WHERE a = 16;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "c2" violates check constraint "c2_a_check""#, detail: "Failing row contains (5, z).", schema: "public", table: "c2", constraint: "c2_a_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt (x INT, a INT, b TEXT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt DROP COLUMN x;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt3 PARTITION OF pt FOR VALUES FROM (20) TO (30) PARTITION BY LIST (b);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt3a PARTITION OF pt3 FOR VALUES IN ('a');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt3b PARTITION OF pt3 FOR VALUES IN ('b');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (1, 'a'), (11, 'b'), (21, 'a'), (22, 'b');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE pt SET b = 'b' WHERE a = 21;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on pt"#)],
                            &[T(r#"  Update on pt3a pt_1"#)],
                            &[T(r#"  Update on pt3b pt_2"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on pt3a pt_1"#)],
                            &[T(r#"              Filter: (a = 21)"#)],
                            &[T(r#"        ->  Seq Scan on pt3b pt_2"#)],
                            &[T(r#"              Filter: (a = 21)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET b = 'b' WHERE a = 21 RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt3b"), T("21"), T("b")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt1"), T("1"), T("a")],
                            &[T("pt2"), T("11"), T("b")],
                            &[T("pt3b"), T("21"), T("b")],
                            &[T("pt3b"), T("22"), T("b")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET a = a + 10 WHERE a < 20 RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt2"), T("11"), T("a")],
                            &[T("pt3b"), T("21"), T("b")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt2"), T("11"), T("a")],
                            &[T("pt3b"), T("21"), T("b")],
                            &[T("pt3b"), T("21"), T("b")],
                            &[T("pt3b"), T("22"), T("b")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET a = 99 WHERE a = 11;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "pt" found for row"#, detail: "Partition key of the failing row contains (a) = (99).", schema: "public", table: "pt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET a = 5, b = 'q' WHERE a = 21 RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt1"), T("5"), T("q")],
                            &[T("pt1"), T("5"), T("q")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt1"), T("5"), T("q")],
                            &[T("pt1"), T("5"), T("q")],
                            &[T("pt2"), T("11"), T("a")],
                            &[T("pt3b"), T("22"), T("b")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pt WHERE b = 'b' RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt3b"), T("22"), T("b")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt1"), T("5"), T("q")],
                            &[T("pt1"), T("5"), T("q")],
                            &[T("pt2"), T("11"), T("a")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM pt WHERE a = (SELECT 5);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Delete on pt"#)],
                            &[T(r#"  Delete on pt1 pt_1"#)],
                            &[T(r#"  Delete on pt2 pt_2"#)],
                            &[T(r#"  Delete on pt3a pt_3"#)],
                            &[T(r#"  Delete on pt3b pt_4"#)],
                            &[T(r#"  InitPlan 1"#)],
                            &[T(r#"    ->  Result"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on pt1 pt_1"#)],
                            &[T(r#"              Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"        ->  Seq Scan on pt2 pt_2"#)],
                            &[T(r#"              Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"        ->  Seq Scan on pt3a pt_3"#)],
                            &[T(r#"              Filter: (a = (InitPlan 1).col1)"#)],
                            &[T(r#"        ->  Seq Scan on pt3b pt_4"#)],
                            &[T(r#"              Filter: (a = (InitPlan 1).col1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pt WHERE a = (SELECT 5);",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ep (a INT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE ep SET a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on ep"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ep SET a = 1;",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ep;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE pt SET b = 'n' WHERE a = 100;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on pt"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE q (a INT, z TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (5, 'q5'), (30, 'q30');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (5, 'f'), (25, 'a');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET b = q.z FROM q WHERE q.a = pt.a + 0 RETURNING pt.tableoid::regclass, pt.*, q.*;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT), Column("a", INT4), Column("z", TEXT)],
                        rows: &[
                            &[T("pt1"), T("5"), T("q5"), T("5"), T("q5")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pt USING q WHERE q.a = pt.a;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pt2"), T("11"), T("a")],
                            &[T("pt3a"), T("25"), T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rows an UPDATE moves to a partition it also changes reuse that partition's result relation",
            set_up_script: &[
                "CREATE TABLE m (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE m1 PARTITION OF m (PRIMARY KEY (a)) FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE m2 PARTITION OF m (PRIMARY KEY (a)) FOR VALUES FROM (10) TO (20);",
                "CREATE TRIGGER nochange BEFORE UPDATE ON m FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();",
                "INSERT INTO m VALUES (1, 'x'), (3, 'y'), (12, 'z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) UPDATE m SET a = CASE WHEN a < 10 THEN a + 10 ELSE a END;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on m (actual rows=0.00 loops=1)"#)],
                            &[T(r#"  Update on m1 m_1"#)],
                            &[T(r#"  Update on m2 m_2"#)],
                            &[T(r#"  ->  Append (actual rows=3.00 loops=1)"#)],
                            &[T(r#"        ->  Seq Scan on m1 m_1 (actual rows=2.00 loops=1)"#)],
                            &[T(r#"        ->  Seq Scan on m2 m_2 (actual rows=1.00 loops=1)"#)],
                            &[T(r#"Trigger nochange on m1: calls=2"#)],
                            &[T(r#"Trigger nochange on m2: calls=1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM m ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m2"), T("11"), T("x")],
                            &[T("m2"), T("12"), T("z")],
                            &[T("m2"), T("13"), T("y")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) UPDATE m SET a = a - 10 WHERE b <> 'z' RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on m (actual rows=2.00 loops=1)"#)],
                            &[T(r#"  Update on m1 m_1"#)],
                            &[T(r#"  Update on m2 m_2"#)],
                            &[T(r#"  ->  Append (actual rows=2.00 loops=1)"#)],
                            &[T(r#"        ->  Seq Scan on m1 m_1 (actual rows=0.00 loops=1)"#)],
                            &[T(r#"              Filter: (b <> 'z'::text)"#)],
                            &[T(r#"        ->  Seq Scan on m2 m_2 (actual rows=2.00 loops=1)"#)],
                            &[T(r#"              Filter: (b <> 'z'::text)"#)],
                            &[T(r#"              Rows Removed by Filter: 1"#)],
                            &[T(r#"Trigger nochange on m2: calls=2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM m ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m1"), T("1"), T("x")],
                            &[T("m1"), T("3"), T("y")],
                            &[T("m2"), T("12"), T("z")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE m SET a = CASE WHEN a < 10 THEN a + 10 ELSE a END RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m2"), T("11"), T("x")],
                            &[T("m2"), T("13"), T("y")],
                        ],
                        tag: "UPDATE 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM m ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m2"), T("11"), T("x")],
                            &[T("m2"), T("12"), T("z")],
                            &[T("m2"), T("13"), T("y")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE m SET a = 5 WHERE a = 12 RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m1"), T("5"), T("z")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE m1 SET a = 13 WHERE a = 5;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "m1" violates partition constraint"#, detail: "Failing row contains (13, z).", schema: "public", table: "m1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE m SET a = 25 WHERE a = 5;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "m" found for row"#, detail: "Partition key of the failing row contains (a) = (25).", schema: "public", table: "m", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM m ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m1"), T("5"), T("z")],
                            &[T("m2"), T("11"), T("x")],
                            &[T("m2"), T("13"), T("y")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM m WHERE a = 11 RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m2"), T("11"), T("x")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM m ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m1"), T("5"), T("z")],
                            &[T("m2"), T("13"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "triggers fire on the partitions an UPDATE moves rows between",
            set_up_script: &[
                "CREATE TABLE tr (a INT, b TEXT) PARTITION BY LIST (a);",
                "CREATE TABLE tr1 PARTITION OF tr FOR VALUES IN (1);",
                "CREATE TABLE tr2 PARTITION OF tr FOR VALUES IN (2);",
                "CREATE FUNCTION trg() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE '% % % on %', TG_WHEN, TG_LEVEL, TG_OP, TG_TABLE_NAME; IF TG_LEVEL = 'ROW' AND TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $$;",
                "CREATE TRIGGER t_bu BEFORE UPDATE ON tr FOR EACH ROW EXECUTE FUNCTION trg();",
                "CREATE TRIGGER t_au AFTER UPDATE ON tr FOR EACH ROW EXECUTE FUNCTION trg();",
                "CREATE TRIGGER t_bd BEFORE DELETE ON tr FOR EACH ROW EXECUTE FUNCTION trg();",
                "CREATE TRIGGER t_ad AFTER DELETE ON tr FOR EACH ROW EXECUTE FUNCTION trg();",
                "CREATE TRIGGER t_bi BEFORE INSERT ON tr FOR EACH ROW EXECUTE FUNCTION trg();",
                "CREATE TRIGGER t_ai AFTER INSERT ON tr FOR EACH ROW EXECUTE FUNCTION trg();",
                "CREATE TRIGGER t_su BEFORE UPDATE ON tr FOR EACH STATEMENT EXECUTE FUNCTION trg();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TRIGGER t_sau AFTER UPDATE ON tr FOR EACH STATEMENT EXECUTE FUNCTION trg();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES (1, 'one');",
                    expected: Expected::Tag("INSERT 0 1"),
                    notices: &[Diagnostic { code: "00000", message: "BEFORE ROW INSERT on tr1", ..N }, Diagnostic { code: "00000", message: "AFTER ROW INSERT on tr1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tr SET b = 'uno' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    notices: &[Diagnostic { code: "00000", message: "BEFORE STATEMENT UPDATE on tr", ..N }, Diagnostic { code: "00000", message: "BEFORE ROW UPDATE on tr1", ..N }, Diagnostic { code: "00000", message: "AFTER ROW UPDATE on tr1", ..N }, Diagnostic { code: "00000", message: "AFTER STATEMENT UPDATE on tr", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tr SET a = 2 WHERE a = 1 RETURNING tableoid::regclass, *;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("tr2"), T("2"), T("uno")],
                        ],
                        tag: "UPDATE 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "BEFORE STATEMENT UPDATE on tr", ..N }, Diagnostic { code: "00000", message: "BEFORE ROW UPDATE on tr1", ..N }, Diagnostic { code: "00000", message: "BEFORE ROW DELETE on tr1", ..N }, Diagnostic { code: "00000", message: "BEFORE ROW INSERT on tr2", ..N }, Diagnostic { code: "00000", message: "AFTER ROW DELETE on tr1", ..N }, Diagnostic { code: "00000", message: "AFTER ROW INSERT on tr2", ..N }, Diagnostic { code: "00000", message: "AFTER STATEMENT UPDATE on tr", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM tr;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("tr2"), T("2"), T("uno")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM tr;",
                    expected: Expected::Tag("DELETE 1"),
                    notices: &[Diagnostic { code: "00000", message: "BEFORE ROW DELETE on tr2", ..N }, Diagnostic { code: "00000", message: "AFTER ROW DELETE on tr2", ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_concurrent_changes_of_rows_moved_to_other_partitions() {
    run_scripts(&[
        ScriptTest {
            name: "READ COMMITTED locks and changes of a row another transaction moved to another partition fail",
            set_up_script: &[
                "CREATE TABLE m (a INT, b TEXT) PARTITION BY RANGE (a)",
                "CREATE TABLE m1 PARTITION OF m (PRIMARY KEY (a)) FOR VALUES FROM (0) TO (10)",
                "CREATE TABLE m2 PARTITION OF m (PRIMARY KEY (a)) FOR VALUES FROM (10) TO (20)",
                "INSERT INTO m VALUES (1, 'a'), (2, 'b')",
                "CREATE TABLE seen (s TEXT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE m SET a = 12 WHERE a = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT a || b FROM m FOR UPDATE",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "tuple to be locked was already moved to another partition due to concurrent update", ..E }),
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
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE m SET a = 3 WHERE a = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE m SET a = 13 WHERE a = 3",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE m SET b = 'z' WHERE b = 'b'",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "tuple to be locked was already moved to another partition due to concurrent update", ..E }),
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
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE m SET a = 4 WHERE a = 12",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM m WHERE b = 'a'",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "tuple to be locked was already moved to another partition due to concurrent update", ..E }),
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
                    query: "/* client B */ SELECT tableoid::regclass, * FROM m ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("m1"), T("4"), T("a")],
                            &[T("m2"), T("13"), T("b")],
                        ],
                        tag: "SELECT 2",
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
fn test_foreign_keys_of_partitioned_tables() {
    run_scripts(&[
        ScriptTest {
            name: "foreign keys from and to partitioned tables have a constraint for each partition and act for rows that move between partitions",
            set_up_script: &[
                "CREATE TABLE pk (a INT PRIMARY KEY, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE pk1 PARTITION OF pk FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pk2 PARTITION OF pk FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pk VALUES (1, 'one'), (2, 'two'), (15, 'fifteen');",
                "CREATE TABLE fk (x INT, a INT REFERENCES pk ON DELETE CASCADE ON UPDATE CASCADE) PARTITION BY LIST (x);",
                "CREATE TABLE fk1 PARTITION OF fk FOR VALUES IN (1);",
                "CREATE TABLE fk2 PARTITION OF fk FOR VALUES IN (2);",
                "INSERT INTO fk VALUES (1, 1), (2, 2), (1, 15);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO fk VALUES (1, 99);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "fk1" violates foreign key constraint "fk_a_fkey""#, detail: r#"Key (a)=(99) is not present in table "pk"."#, schema: "public", table: "fk1", constraint: "fk_a_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, confrelid::regclass, conparentid <> 0 AS child, conislocal, coninhcount, connoinherit FROM pg_constraint WHERE contype = 'f' ORDER BY conrelid::regclass::text, confrelid::regclass::text, conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("confrelid", REGCLASS), Column("child", BOOL), Column("conislocal", BOOL), Column("coninhcount", INT2), Column("connoinherit", BOOL)],
                        rows: &[
                            &[T("fk_a_fkey"), T("fk"), T("pk"), T("f"), T("t"), T("0"), T("f")],
                            &[T("fk_a_fkey_1"), T("fk"), T("pk1"), T("t"), T("f"), T("1"), T("f")],
                            &[T("fk_a_fkey_2"), T("fk"), T("pk2"), T("t"), T("f"), T("1"), T("f")],
                            &[T("fk_a_fkey"), T("fk1"), T("pk"), T("t"), T("f"), T("1"), T("f")],
                            &[T("fk_a_fkey"), T("fk2"), T("pk"), T("t"), T("f"), T("1"), T("f")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass, tgfoid::regproc, tgparentid <> 0 AS child, tgconstrrelid::regclass FROM pg_trigger WHERE tgconstraint <> 0 ORDER BY tgrelid::regclass::text, tgfoid::regproc::text, tgconstrrelid::regclass::text;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", REGCLASS), Column("tgfoid", REGPROC), Column("child", BOOL), Column("tgconstrrelid", REGCLASS)],
                        rows: &[
                            &[T("fk"), T(r#""RI_FKey_check_ins""#), T("f"), T("pk")],
                            &[T("fk"), T(r#""RI_FKey_check_upd""#), T("f"), T("pk")],
                            &[T("fk1"), T(r#""RI_FKey_check_ins""#), T("t"), T("pk")],
                            &[T("fk1"), T(r#""RI_FKey_check_upd""#), T("t"), T("pk")],
                            &[T("fk2"), T(r#""RI_FKey_check_ins""#), T("t"), T("pk")],
                            &[T("fk2"), T(r#""RI_FKey_check_upd""#), T("t"), T("pk")],
                            &[T("pk"), T(r#""RI_FKey_cascade_del""#), T("f"), T("fk")],
                            &[T("pk"), T(r#""RI_FKey_cascade_upd""#), T("f"), T("fk")],
                            &[T("pk1"), T(r#""RI_FKey_cascade_del""#), T("t"), T("fk")],
                            &[T("pk1"), T(r#""RI_FKey_cascade_upd""#), T("t"), T("fk")],
                            &[T("pk2"), T(r#""RI_FKey_cascade_del""#), T("t"), T("fk")],
                            &[T("pk2"), T(r#""RI_FKey_cascade_upd""#), T("t"), T("fk")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pk WHERE a = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fk ORDER BY x, a;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("15")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pk SET a = 3 WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fk ORDER BY x, a;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("1"), T("15")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pk SET a = 16 WHERE a = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pk ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("pk2"), T("15"), T("fifteen")],
                            &[T("pk2"), T("16"), T("one")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM fk ORDER BY x, a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("x", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("fk1"), T("1"), T("15")],
                            &[T("fk1"), T("1"), T("16")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pk3 PARTITION OF pk FOR VALUES FROM (20) TO (30);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fk3 PARTITION OF fk FOR VALUES IN (3);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pk VALUES (25, 'twenty-five');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk VALUES (3, 25);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk VALUES (3, 26);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "fk3" violates foreign key constraint "fk_a_fkey""#, detail: r#"Key (a)=(26) is not present in table "pk"."#, schema: "public", table: "fk3", constraint: "fk_a_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, confrelid::regclass, conparentid <> 0 AS child FROM pg_constraint WHERE contype = 'f' ORDER BY conrelid::regclass::text, confrelid::regclass::text, conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("confrelid", REGCLASS), Column("child", BOOL)],
                        rows: &[
                            &[T("fk_a_fkey"), T("fk"), T("pk"), T("f")],
                            &[T("fk_a_fkey_1"), T("fk"), T("pk1"), T("t")],
                            &[T("fk_a_fkey_2"), T("fk"), T("pk2"), T("t")],
                            &[T("fk_a_fkey_3"), T("fk"), T("pk3"), T("t")],
                            &[T("fk_a_fkey"), T("fk1"), T("pk"), T("t")],
                            &[T("fk_a_fkey"), T("fk2"), T("pk"), T("t")],
                            &[T("fk_a_fkey"), T("fk3"), T("pk"), T("t")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk1 DROP CONSTRAINT fk_a_fkey;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"cannot drop inherited constraint "fk_a_fkey" of relation "fk1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk1 ALTER CONSTRAINT fk_a_fkey DEFERRABLE;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot alter constraint "fk_a_fkey" on relation "fk1""#, detail: r#"Constraint "fk_a_fkey" is derived from constraint "fk_a_fkey" of relation "fk"."#, hint: "You may alter the constraint it derives from instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk ALTER CONSTRAINT fk_a_fkey DEFERRABLE INITIALLY DEFERRED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, condeferrable, condeferred FROM pg_constraint WHERE contype = 'f' ORDER BY conrelid::regclass::text, confrelid::regclass::text, conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("condeferrable", BOOL), Column("condeferred", BOOL)],
                        rows: &[
                            &[T("fk_a_fkey"), T("fk"), T("t"), T("t")],
                            &[T("fk_a_fkey_1"), T("fk"), T("t"), T("t")],
                            &[T("fk_a_fkey_2"), T("fk"), T("t"), T("t")],
                            &[T("fk_a_fkey_3"), T("fk"), T("t"), T("t")],
                            &[T("fk_a_fkey"), T("fk1"), T("t"), T("t")],
                            &[T("fk_a_fkey"), T("fk2"), T("t"), T("t")],
                            &[T("fk_a_fkey"), T("fk3"), T("t"), T("t")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk VALUES (1, 27);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pk VALUES (27, 'late');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fk ORDER BY x, a;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("15")],
                            &[T("1"), T("16")],
                            &[T("1"), T("27")],
                            &[T("3"), T("25")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pk1;",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pk WHERE a = 27;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plain (a INT REFERENCES pk);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO plain VALUES (16);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pk WHERE a = 16;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"update or delete on table "pk2" violates foreign key constraint "plain_a_fkey_2" on table "plain""#, detail: r#"Key (a)=(16) is still referenced from table "plain"."#, schema: "public", table: "plain", constraint: "plain_a_fkey_2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pk SET a = 17 WHERE a = 16;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"update or delete on table "pk2" violates foreign key constraint "plain_a_fkey_2" on table "plain""#, detail: r#"Key (a)=(16) is still referenced from table "plain"."#, schema: "public", table: "plain", constraint: "plain_a_fkey_2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pk SET a = 5 WHERE a = 16;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"update or delete on table "pk" violates foreign key constraint "plain_a_fkey" on table "plain""#, detail: r#"Key (a)=(16) is still referenced from table "plain"."#, schema: "public", table: "plain", constraint: "plain_a_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE fk SET x = 2 WHERE a = 15;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE fk SET x = 1, a = 99 WHERE a = 15;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "fk1" violates foreign key constraint "fk_a_fkey""#, detail: r#"Key (a)=(99) is not present in table "pk"."#, schema: "public", table: "fk1", constraint: "fk_a_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM fk ORDER BY x, a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("x", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("fk1"), T("1"), T("16")],
                            &[T("fk2"), T("2"), T("15")],
                            &[T("fk3"), T("3"), T("25")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk DROP CONSTRAINT fk_a_fkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_constraint WHERE contype = 'f' AND conrelid IN ('fk'::regclass, 'fk1'::regclass, 'fk2'::regclass, 'fk3'::regclass);",
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
                    query: "SELECT count(*) FROM pg_trigger WHERE tgconstraint <> 0 AND tgconstrrelid IN ('fk'::regclass, 'fk1'::regclass, 'fk2'::regclass, 'fk3'::regclass);",
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
                    query: "CREATE TABLE q (a INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk1 ADD CONSTRAINT fk1_q FOREIGN KEY (a) REFERENCES q;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "fk1" violates foreign key constraint "fk1_q""#, detail: r#"Key (a)=(16) is not present in table "q"."#, schema: "public", table: "fk1", constraint: "fk1_q", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk ADD CONSTRAINT fk_q FOREIGN KEY (a) REFERENCES q;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "fk1" violates foreign key constraint "fk_q""#, detail: r#"Key (a)=(16) is not present in table "q"."#, schema: "public", table: "fk1", constraint: "fk_q", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM fk WHERE a NOT IN (1, 2);",
                    expected: Expected::Tag("DELETE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk ADD CONSTRAINT fk_q FOREIGN KEY (a) REFERENCES q;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, confrelid::regclass, conparentid <> 0 AS child, conislocal, coninhcount FROM pg_constraint WHERE contype = 'f' ORDER BY conrelid::regclass::text, confrelid::regclass::text, conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("confrelid", REGCLASS), Column("child", BOOL), Column("conislocal", BOOL), Column("coninhcount", INT2)],
                        rows: &[
                            &[T("fk_q"), T("fk"), T("q"), T("f"), T("t"), T("0")],
                            &[T("fk_q"), T("fk1"), T("q"), T("t"), T("f"), T("1")],
                            &[T("fk_q"), T("fk2"), T("q"), T("t"), T("f"), T("1")],
                            &[T("fk_q"), T("fk3"), T("q"), T("t"), T("f"), T("1")],
                            &[T("plain_a_fkey"), T("plain"), T("pk"), T("f"), T("t"), T("0")],
                            &[T("plain_a_fkey_1"), T("plain"), T("pk1"), T("t"), T("f"), T("1")],
                            &[T("plain_a_fkey_2"), T("plain"), T("pk2"), T("t"), T("f"), T("1")],
                            &[T("plain_a_fkey_3"), T("plain"), T("pk3"), T("t"), T("f"), T("1")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk ADD CONSTRAINT fk_q_nv FOREIGN KEY (a) REFERENCES q NOT VALID;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk VALIDATE CONSTRAINT fk_q_nv;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, convalidated FROM pg_constraint WHERE conname LIKE 'fk_q_nv%' ORDER BY 2;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("convalidated", BOOL)],
                        rows: &[
                            &[T("fk_q_nv"), T("fk"), T("t")],
                            &[T("fk_q_nv"), T("fk1"), T("t")],
                            &[T("fk_q_nv"), T("fk2"), T("t")],
                            &[T("fk_q_nv"), T("fk3"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk1 ADD CONSTRAINT fk1_own FOREIGN KEY (a) REFERENCES q;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk ADD CONSTRAINT fk_own FOREIGN KEY (a) REFERENCES q;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pk VALUES (1, 'one'), (2, 'two');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk1 ADD CONSTRAINT fk1_pk FOREIGN KEY (a) REFERENCES pk;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fk ADD CONSTRAINT fk_pk FOREIGN KEY (a) REFERENCES pk;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, confrelid::regclass, conparentid <> 0 AS child, conislocal, coninhcount FROM pg_constraint WHERE conname LIKE 'fk%own%' OR conname LIKE 'fk%pk%' ORDER BY 2, 3, 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("confrelid", REGCLASS), Column("child", BOOL), Column("conislocal", BOOL), Column("coninhcount", INT2)],
                        rows: &[
                            &[T("fk_pk"), T("fk"), T("pk"), T("f"), T("t"), T("0")],
                            &[T("fk_pk_1"), T("fk"), T("pk1"), T("t"), T("f"), T("1")],
                            &[T("fk_pk_2"), T("fk"), T("pk2"), T("t"), T("f"), T("1")],
                            &[T("fk_pk_3"), T("fk"), T("pk3"), T("t"), T("f"), T("1")],
                            &[T("fk_own"), T("fk"), T("q"), T("f"), T("t"), T("0")],
                            &[T("fk1_pk"), T("fk1"), T("pk"), T("t"), T("f"), T("1")],
                            &[T("fk1_own"), T("fk1"), T("q"), T("t"), T("f"), T("1")],
                            &[T("fk_pk"), T("fk2"), T("pk"), T("t"), T("f"), T("1")],
                            &[T("fk_own"), T("fk2"), T("q"), T("t"), T("f"), T("1")],
                            &[T("fk_pk"), T("fk3"), T("pk"), T("t"), T("f"), T("1")],
                            &[T("fk_own"), T("fk3"), T("q"), T("t"), T("f"), T("1")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass, tgfoid::regproc, tgparentid <> 0 AS child FROM pg_trigger t JOIN pg_constraint c ON c.oid = t.tgconstraint WHERE c.conname LIKE 'fk%own%' OR c.conname LIKE 'fk%pk%' ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", REGCLASS), Column("tgfoid", REGPROC), Column("child", BOOL)],
                        rows: &[
                            &[T("pk"), T(r#""RI_FKey_noaction_del""#), T("f")],
                            &[T("pk"), T(r#""RI_FKey_noaction_upd""#), T("f")],
                            &[T("pk1"), T(r#""RI_FKey_noaction_del""#), T("t")],
                            &[T("pk1"), T(r#""RI_FKey_noaction_upd""#), T("t")],
                            &[T("pk2"), T(r#""RI_FKey_noaction_del""#), T("t")],
                            &[T("pk2"), T(r#""RI_FKey_noaction_upd""#), T("t")],
                            &[T("fk"), T(r#""RI_FKey_check_ins""#), T("f")],
                            &[T("fk"), T(r#""RI_FKey_check_ins""#), T("f")],
                            &[T("fk"), T(r#""RI_FKey_check_upd""#), T("f")],
                            &[T("fk"), T(r#""RI_FKey_check_upd""#), T("f")],
                            &[T("fk1"), T(r#""RI_FKey_check_ins""#), T("t")],
                            &[T("fk1"), T(r#""RI_FKey_check_ins""#), T("t")],
                            &[T("fk1"), T(r#""RI_FKey_check_upd""#), T("t")],
                            &[T("fk1"), T(r#""RI_FKey_check_upd""#), T("t")],
                            &[T("fk2"), T(r#""RI_FKey_check_ins""#), T("t")],
                            &[T("fk2"), T(r#""RI_FKey_check_ins""#), T("t")],
                            &[T("fk2"), T(r#""RI_FKey_check_upd""#), T("t")],
                            &[T("fk2"), T(r#""RI_FKey_check_upd""#), T("t")],
                            &[T("pk3"), T(r#""RI_FKey_noaction_del""#), T("t")],
                            &[T("pk3"), T(r#""RI_FKey_noaction_upd""#), T("t")],
                            &[T("fk3"), T(r#""RI_FKey_check_ins""#), T("t")],
                            &[T("fk3"), T(r#""RI_FKey_check_ins""#), T("t")],
                            &[T("fk3"), T(r#""RI_FKey_check_upd""#), T("t")],
                            &[T("fk3"), T(r#""RI_FKey_check_upd""#), T("t")],
                            &[T("q"), T(r#""RI_FKey_noaction_del""#), T("f")],
                            &[T("q"), T(r#""RI_FKey_noaction_upd""#), T("f")],
                        ],
                        tag: "SELECT 26",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE deep (a INT PRIMARY KEY) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE deep1 PARTITION OF deep FOR VALUES FROM (0) TO (100) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE deep11 PARTITION OF deep1 FOR VALUES FROM (0) TO (50);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE deep12 PARTITION OF deep1 FOR VALUES FROM (50) TO (100);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE deep2 PARTITION OF deep FOR VALUES FROM (100) TO (200);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO deep VALUES (10), (60);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE refs_mid (a INT REFERENCES deep1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO refs_mid VALUES (60);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE deep SET a = 150 WHERE a = 10;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot move tuple across partitions when a non-root ancestor of the source partition is directly referenced in a foreign key", detail: r#"A foreign key points to ancestor "deep1" but not the root ancestor "deep"."#, hint: r#"Consider defining the foreign key on table "deep"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE deep SET a = 70 WHERE a = 60;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"update or delete on table "deep12" violates foreign key constraint "refs_mid_a_fkey_2" on table "refs_mid""#, detail: r#"Key (a)=(60) is still referenced from table "refs_mid"."#, schema: "public", table: "refs_mid", constraint: "refs_mid_a_fkey_2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM deep ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("deep11"), T("10")],
                            &[T("deep12"), T("60")],
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
fn test_attach_and_detach_partition() {
    run_scripts(&[
        ScriptTest {
            name: "ATTACH and DETACH PARTITION move tables into and out of partitioned tables, checking their rows, indexes, triggers, foreign keys, and identity columns",
            set_up_script: &[
                "CREATE TABLE p (a INT NOT NULL, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE t2 (a INT NOT NULL, b TEXT);",
                "INSERT INTO t2 VALUES (10, 'ten'), (15, 'fifteen');",
                "ALTER TABLE p ATTACH PARTITION t2 FOR VALUES FROM (10) TO (20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("t2"), T("10"), T("ten")],
                            &[T("t2"), T("15"), T("fifteen")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relispartition, relpartbound IS NOT NULL AS bound FROM pg_class WHERE relname IN ('p1', 't2') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relispartition", BOOL), Column("bound", BOOL)],
                        rows: &[
                            &[T("p1"), T("t"), T("t")],
                            &[T("t2"), T("t"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attislocal, attinhcount FROM pg_attribute WHERE attrelid = 't2'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attislocal", BOOL), Column("attinhcount", INT2)],
                        rows: &[
                            &[T("a"), T("f"), T("1")],
                            &[T("b"), T("f"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conislocal, coninhcount FROM pg_constraint WHERE conrelid = 't2'::regclass ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conislocal", BOOL), Column("coninhcount", INT2)],
                        rows: &[
                            &[T("t2_a_not_null"), T("f"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (a INT NOT NULL, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3 VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t3 FOR VALUES FROM (20) TO (30);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"partition constraint of relation "t3" is violated by some row"#, schema: "public", table: "t3", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t3 FOR VALUES FROM (15) TO (30);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"partition "t3" would overlap partition "t2""#, position: 52, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3 VALUES (25);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t3 FOR VALUES FROM (20) TO (30);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t3 FOR VALUES FROM (40) TO (50);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""t3" is already a partition"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t4 (a INT NOT NULL, b TEXT, c INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t4 FOR VALUES FROM (30) TO (40);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"table "t4" contains column "c" not found in parent "p""#, detail: "The new partition may contain only the columns present in parent.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t5 (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t5 FOR VALUES FROM (30) TO (40);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"column "a" in child table "t5" must be marked NOT NULL"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t6 (a INT NOT NULL, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t6 FOR VALUES FROM (30) TO (40);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"child table "t6" has different type for column "b""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t7 (b TEXT, a INT NOT NULL CHECK (a >= 30 AND a < 40));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t7 VALUES ('thirty', 30);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t7 FOR VALUES FROM (30) TO (40);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("t2"), T("10"), T("ten")],
                            &[T("t2"), T("15"), T("fifteen")],
                            &[T("t3"), T("25"), Null],
                            &[T("t7"), T("30"), T("thirty")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE notpart (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE notpart ATTACH PARTITION t5 FOR VALUES FROM (0) TO (1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action ATTACH PARTITION cannot be performed on relation "notpart""#, detail: "This operation is not supported for tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION p1 FOR VALUES FROM (100) TO (200);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""p1" is already a partition"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE inhparent (a INT NOT NULL, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE inhchild () INHERITS (inhparent);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION inhchild FOR VALUES FROM (100) TO (200);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot attach inheritance child as partition", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION inhparent FOR VALUES FROM (100) TO (200);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot attach inheritance parent as partition", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION p FOR VALUES FROM (100) TO (200);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "circular inheritance not allowed", detail: r#""p" is already a child of "p"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pd PARTITION OF p DEFAULT;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (55, 'fifty-five'), (75, 'seventy-five');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t8 (a INT NOT NULL, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t8 FOR VALUES FROM (50) TO (60);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"updated partition constraint for default partition "pd" would be violated by some row"#, schema: "public", table: "pd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pd WHERE a = 55;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t8 FOR VALUES FROM (50) TO (60);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (55, 'fifty-five');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("t2"), T("10"), T("ten")],
                            &[T("t2"), T("15"), T("fifteen")],
                            &[T("t3"), T("25"), Null],
                            &[T("t7"), T("30"), T("thirty")],
                            &[T("t8"), T("55"), T("fifty-five")],
                            &[T("pd"), T("75"), T("seventy-five")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p DETACH PARTITION t8;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("t2"), T("10"), T("ten")],
                            &[T("t2"), T("15"), T("fifteen")],
                            &[T("t3"), T("25"), Null],
                            &[T("t7"), T("30"), T("thirty")],
                            &[T("pd"), T("75"), T("seventy-five")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relispartition, relpartbound IS NULL AS nobound FROM pg_class WHERE relname = 't8';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relispartition", BOOL), Column("nobound", BOOL)],
                        rows: &[
                            &[T("t8"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attislocal, attinhcount FROM pg_attribute WHERE attrelid = 't8'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attislocal", BOOL), Column("attinhcount", INT2)],
                        rows: &[
                            &[T("a"), T("t"), T("0")],
                            &[T("b"), T("t"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t8 VALUES (999, 'anything');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p DETACH PARTITION t8;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "t8" is not a partition of relation "p""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p DETACH PARTITION notpart;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "notpart" is not a partition of relation "p""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p DETACH PARTITION pd;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (85, 'eighty-five');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "p" found for row"#, detail: "Partition key of the failing row contains (a) = (85).", schema: "public", table: "p", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION pd DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (85, 'eighty-five');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p DETACH PARTITION t8 CONCURRENTLY;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot detach partitions concurrently when a default partition exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p DETACH PARTITION t2 FINALIZE;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot complete detaching partition "t2""#, detail: "There's no pending concurrent detach.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 DETACH PARTITION t3;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action DETACH PARTITION cannot be performed on relation "t2""#, detail: "This operation is not supported for tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ONLY p DETACH PARTITION t2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("t3"), T("25"), Null],
                            &[T("t7"), T("30"), T("thirty")],
                            &[T("pd"), T("75"), T("seventy-five")],
                            &[T("pd"), T("85"), T("eighty-five")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE q (a INT PRIMARY KEY, b TEXT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX q_b ON q (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE q1 (a INT NOT NULL, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX q1_b ON q1 (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE q ATTACH PARTITION q1 FOR VALUES IN (1, 2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, i.inhparent::regclass FROM pg_class c LEFT JOIN pg_inherits i ON i.inhrelid = c.oid WHERE c.relname LIKE 'q1%' ORDER BY c.relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("inhparent", REGCLASS)],
                        rows: &[
                            &[T("q1"), T("q")],
                            &[T("q1_b"), T("q_b")],
                            &[T("q1_pkey"), T("q_pkey")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, conparentid <> 0 AS child FROM pg_constraint WHERE conrelid IN ('q'::regclass, 'q1'::regclass) AND contype = 'p' ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("child", BOOL)],
                        rows: &[
                            &[T("q1_pkey"), T("q1"), T("t")],
                            &[T("q_pkey"), T("q"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (1, 'one');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (1, 'uno');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "q1_pkey""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "q1", constraint: "q1_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE q DETACH PARTITION q1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, i.inhparent::regclass FROM pg_class c LEFT JOIN pg_inherits i ON i.inhrelid = c.oid WHERE c.relname LIKE 'q1%' ORDER BY c.relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("inhparent", REGCLASS)],
                        rows: &[
                            &[T("q1"), Null],
                            &[T("q1_b"), Null],
                            &[T("q1_pkey"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, conparentid <> 0 AS child FROM pg_constraint WHERE conrelid IN ('q'::regclass, 'q1'::regclass) AND contype = 'p' ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("child", BOOL)],
                        rows: &[
                            &[T("q1_pkey"), T("q1"), T("f")],
                            &[T("q_pkey"), T("q"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX q1_b;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX q1_pkey;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop index q1_pkey because constraint q1_pkey on table q1 requires it", hint: "You can drop constraint q1_pkey on table q1 instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE q1 DROP CONSTRAINT q1_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r (a INT, b INT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX r_b ON ONLY r (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r1 PARTITION OF r FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE r2 PARTITION OF r FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass, indisvalid FROM pg_index WHERE indrelid = 'r'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("r_b"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX r1_b ON r1 (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX r_b ATTACH PARTITION r1_b;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot attach index "r1_b" as a partition of index "r_b""#, detail: r#"Another index is already attached for partition "r1"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass, indisvalid FROM pg_index WHERE indrelid = 'r'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("r_b"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX r_b ATTACH PARTITION r1_b;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot attach index "r1_b" as a partition of index "r_b""#, detail: r#"Another index is already attached for partition "r1"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX r2_a ON r2 (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX r_b ATTACH PARTITION r2_a;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot attach index "r2_a" as a partition of index "r_b""#, detail: r#"Another index is already attached for partition "r2"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX r2_b ON r2 (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX r2_b2 ON r2 (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX r_b ATTACH PARTITION r2_b;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot attach index "r2_b" as a partition of index "r_b""#, detail: r#"Another index is already attached for partition "r2"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX r_b ATTACH PARTITION r2_b2;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot attach index "r2_b2" as a partition of index "r_b""#, detail: r#"Another index is already attached for partition "r2"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass, indisvalid FROM pg_index WHERE indrelid = 'r'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("r_b"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX r_b ATTACH PARTITION q1;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#""q1" is not an index"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX r1_b ATTACH PARTITION r2_b;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action ATTACH PARTITION cannot be performed on relation "r1_b""#, detail: "This operation is not supported for indexes.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX r_b ATTACH PARTITION nosuchindex;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuchindex" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ip (a INT GENERATED ALWAYS AS IDENTITY, b TEXT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ip1 PARTITION OF ip FOR VALUES FROM (1) TO (100);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ip (b) VALUES ('x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ip1 (b) VALUES ('y');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ip2 (a INT NOT NULL, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ip ATTACH PARTITION ip2 FOR VALUES FROM (100) TO (200);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attrelid::regclass, attname, attidentity FROM pg_attribute WHERE attrelid IN ('ip'::regclass, 'ip1'::regclass, 'ip2'::regclass) AND attnum = 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("attrelid", REGCLASS), Column("attname", NAME), Column("attidentity", CHAR)],
                        rows: &[
                            &[T("ip"), T("a"), T("a")],
                            &[T("ip1"), T("a"), T("a")],
                            &[T("ip2"), T("a"), T("a")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ip ALTER COLUMN a SET GENERATED BY DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attrelid::regclass, attname, attidentity FROM pg_attribute WHERE attrelid IN ('ip'::regclass, 'ip1'::regclass, 'ip2'::regclass) AND attnum = 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("attrelid", REGCLASS), Column("attname", NAME), Column("attidentity", CHAR)],
                        rows: &[
                            &[T("ip"), T("a"), T("d")],
                            &[T("ip1"), T("a"), T("d")],
                            &[T("ip2"), T("a"), T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ip1 ALTER COLUMN a DROP IDENTITY;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "cannot drop identity from a column of a partition", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ip DETACH PARTITION ip1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attrelid::regclass, attname, attidentity FROM pg_attribute WHERE attrelid IN ('ip'::regclass, 'ip1'::regclass, 'ip2'::regclass) AND attnum = 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("attrelid", REGCLASS), Column("attname", NAME), Column("attidentity", CHAR)],
                        rows: &[
                            &[T("ip"), T("a"), T("d")],
                            &[T("ip1"), T("a"), T("")],
                            &[T("ip2"), T("a"), T("d")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ip (b) VALUES ('z');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "ip" found for row"#, detail: "Partition key of the failing row contains (a) = (3).", schema: "public", table: "ip", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM ip ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ip3 (a INT GENERATED ALWAYS AS IDENTITY, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ip ATTACH PARTITION ip3 FOR VALUES FROM (200) TO (300);",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"table "ip3" being attached contains an identity column "a""#, detail: "The new partition may not contain an identity column.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ip ALTER COLUMN a DROP IDENTITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attrelid::regclass, attname, attidentity FROM pg_attribute WHERE attrelid IN ('ip'::regclass, 'ip2'::regclass) AND attnum = 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("attrelid", REGCLASS), Column("attname", NAME), Column("attidentity", CHAR)],
                        rows: &[
                            &[T("ip"), T("a"), T("")],
                            &[T("ip2"), T("a"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fpk (a INT PRIMARY KEY) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fpk1 PARTITION OF fpk FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fpk2 PARTITION OF fpk FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fpk VALUES (1), (15);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ffk (a INT REFERENCES fpk);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ffk VALUES (15);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fpk DETACH PARTITION fpk2;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"removing partition "fpk2" violates foreign key constraint "ffk_a_fkey_2""#, detail: r#"Key (a)=(15) is still referenced from table "ffk"."#, schema: "public", table: "ffk", constraint: "ffk_a_fkey_2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ffk;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fpk DETACH PARTITION fpk2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, confrelid::regclass, conparentid <> 0 AS child FROM pg_constraint WHERE contype = 'f' AND conrelid = 'ffk'::regclass ORDER BY 3;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("confrelid", REGCLASS), Column("child", BOOL)],
                        rows: &[
                            &[T("ffk_a_fkey"), T("ffk"), T("fpk"), T("f")],
                            &[T("ffk_a_fkey_1"), T("ffk"), T("fpk1"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ffp (a INT REFERENCES fpk) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ffp1 PARTITION OF ffp FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ffp VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ffp DETACH PARTITION ffp1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, confrelid::regclass, conparentid <> 0 AS child, conislocal, coninhcount FROM pg_constraint WHERE contype = 'f' AND conrelid = 'ffp1'::regclass ORDER BY 3;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("confrelid", REGCLASS), Column("child", BOOL), Column("conislocal", BOOL), Column("coninhcount", INT2)],
                        rows: &[
                            &[T("ffp_a_fkey"), T("ffp1"), T("fpk"), T("f"), T("t"), T("0")],
                            &[T("ffp_a_fkey_2"), T("ffp1"), T("fpk1"), T("t"), T("f"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass, tgfoid::regproc, tgparentid <> 0 AS child FROM pg_trigger WHERE tgconstraint IN (SELECT oid FROM pg_constraint WHERE conrelid = 'ffp1'::regclass) ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", REGCLASS), Column("tgfoid", REGPROC), Column("child", BOOL)],
                        rows: &[
                            &[T("fpk"), T(r#""RI_FKey_noaction_del""#), T("f")],
                            &[T("fpk"), T(r#""RI_FKey_noaction_upd""#), T("f")],
                            &[T("fpk1"), T(r#""RI_FKey_noaction_del""#), T("t")],
                            &[T("fpk1"), T(r#""RI_FKey_noaction_upd""#), T("t")],
                            &[T("ffp1"), T(r#""RI_FKey_check_ins""#), T("f")],
                            &[T("ffp1"), T(r#""RI_FKey_check_upd""#), T("f")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ffp1 VALUES (2);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "ffp1" violates foreign key constraint "ffp_a_fkey""#, detail: r#"Key (a)=(2) is not present in table "fpk"."#, schema: "public", table: "ffp1", constraint: "ffp_a_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM fpk WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"update or delete on table "fpk1" violates foreign key constraint "ffp_a_fkey_2" on table "ffp1""#, detail: r#"Key (a)=(1) is still referenced from table "ffp1"."#, schema: "public", table: "ffp1", constraint: "ffp_a_fkey_2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ffp2 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ffp2 VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ffp ATTACH PARTITION ffp2 FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "ffp2" violates foreign key constraint "ffp_a_fkey""#, detail: r#"Key (a)=(3) is not present in table "fpk"."#, schema: "public", table: "ffp2", constraint: "ffp_a_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ffp2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ffp ATTACH PARTITION ffp2 FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, confrelid::regclass, conparentid <> 0 AS child FROM pg_constraint WHERE contype = 'f' AND conrelid = 'ffp2'::regclass ORDER BY 3;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("confrelid", REGCLASS), Column("child", BOOL)],
                        rows: &[
                            &[T("ffp_a_fkey"), T("ffp2"), T("fpk"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE trp (a INT, b TEXT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION trf() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'trigger on %', TG_TABLE_NAME; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER trt BEFORE INSERT ON trp FOR EACH ROW EXECUTE FUNCTION trf();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE trp1 (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE trp ATTACH PARTITION trp1 FOR VALUES IN (1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO trp VALUES (1, 'one');",
                    expected: Expected::Tag("INSERT 0 1"),
                    notices: &[Diagnostic { code: "00000", message: "trigger on trp1", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass, tgname, tgparentid <> 0 AS child FROM pg_trigger WHERE tgname = 'trt' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", REGCLASS), Column("tgname", NAME), Column("child", BOOL)],
                        rows: &[
                            &[T("trp"), T("trt"), T("f")],
                            &[T("trp1"), T("trt"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE trp DETACH PARTITION trp1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass, tgname, tgparentid <> 0 AS child FROM pg_trigger WHERE tgname = 'trt' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", REGCLASS), Column("tgname", NAME), Column("child", BOOL)],
                        rows: &[
                            &[T("trp"), T("trt"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO trp1 VALUES (1, 'uno');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE trp2 (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER trt BEFORE INSERT ON trp2 FOR EACH ROW EXECUTE FUNCTION trf();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE trp ATTACH PARTITION trp2 FOR VALUES IN (2);",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"trigger "trt" for relation "trp2" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck (a INT, b INT CHECK (b > 0)) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck1 (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ck ATTACH PARTITION ck1 FOR VALUES IN (1);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"child table is missing constraint "ck_b_check""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck2 (a INT, b INT, CONSTRAINT ck_b_check CHECK (b > 0));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ck ATTACH PARTITION ck2 FOR VALUES IN (2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, conislocal, coninhcount FROM pg_constraint WHERE conrelid = 'ck2'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("conislocal", BOOL), Column("coninhcount", INT2)],
                        rows: &[
                            &[T("ck_b_check"), T("ck2"), T("f"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ck DETACH PARTITION ck2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, conrelid::regclass, conislocal, coninhcount FROM pg_constraint WHERE conrelid = 'ck2'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("conrelid", REGCLASS), Column("conislocal", BOOL), Column("coninhcount", INT2)],
                        rows: &[
                            &[T("ck_b_check"), T("ck2"), T("t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE mp (a INT, b INT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE mp1 (a INT, b INT) PARTITION BY RANGE (b);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE mp11 PARTITION OF mp1 FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mp1 VALUES (100, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mp ATTACH PARTITION mp1 FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"partition constraint of relation "mp11" is violated by some row"#, schema: "public", table: "mp11", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM mp1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mp1 VALUES (5, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mp ATTACH PARTITION mp1 FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM mp;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("mp11"), T("5"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mp11 DETACH PARTITION mp1;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action DETACH PARTITION cannot be performed on relation "mp11""#, detail: "This operation is not supported for tables.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE mp1 ATTACH PARTITION mp FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "circular inheritance not allowed", detail: r#""mp1" is already a child of "mp"."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lp (a INT, b TEXT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lpd PARTITION OF lp DEFAULT;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lp VALUES (NULL, 'null'), (3, 'three');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lp1 (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE lp ATTACH PARTITION lp1 FOR VALUES IN (1, 2, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"updated partition constraint for default partition "lpd" would be violated by some row"#, schema: "public", table: "lpd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM lp WHERE a IS NULL;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE lp ATTACH PARTITION lp1 FOR VALUES IN (1, 2, NULL);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lpd2 (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE lp ATTACH PARTITION lpd2 DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"partition "lpd2" conflicts with existing default partition "lpd""#, position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE hp (a INT) PARTITION BY HASH (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE hp0 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO hp0 VALUES (1), (2), (3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE hp ATTACH PARTITION hp0 FOR VALUES WITH (MODULUS 2, REMAINDER 0);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"partition constraint of relation "hp0" is violated by some row"#, schema: "public", table: "hp0", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE hp1 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE hp ATTACH PARTITION hp1 FOR VALUES WITH (MODULUS 1, REMAINDER 0);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE hp ATTACH PARTITION hp1 FOR VALUES WITH (MODULUS 2, REMAINDER 1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""hp1" is already a partition"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM hp ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE tmp1 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ptemp (a INT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ptemp ATTACH PARTITION tmp1 FOR VALUES IN (1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"cannot attach a temporary relation as partition of permanent relation "ptemp""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE tt AS (a INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE typed OF tt;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ptemp ATTACH PARTITION typed FOR VALUES IN (1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot attach a typed table as partition", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT 1 AS a;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ptemp ATTACH PARTITION v FOR VALUES IN (1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action ATTACH PARTITION cannot be performed on relation "v""#, detail: "This operation is not supported for views.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE v ATTACH PARTITION ptemp FOR VALUES IN (1);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action ATTACH PARTITION cannot be performed on relation "v""#, detail: "This operation is not supported for views.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION t5 DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"partition "t5" conflicts with existing default partition "pd""#, position: 35, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b1 (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION b1 FOR VALUES IN (1);",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "invalid bound specification for a range partition", position: 46, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION b1 FOR VALUES FROM ('x') TO ('y');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "x""#, position: 52, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_default_partition_contents_checked_for_new_partitions() {
    run_scripts(&[
        ScriptTest {
            name: "a new partition checks that its parent's default partition, and that partition's own partitions, hold no rows of its bound",
            set_up_script: &[
                "CREATE TABLE dp (a INT, b TEXT) PARTITION BY LIST (a);",
                "CREATE TABLE dp1 PARTITION OF dp FOR VALUES IN (1);",
                "CREATE TABLE dpd PARTITION OF dp DEFAULT;",
                "INSERT INTO dp VALUES (1, 'one'), (2, 'two'), (3, 'three'), (NULL, 'null');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE dp2 PARTITION OF dp FOR VALUES IN (2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"updated partition constraint for default partition "dpd" would be violated by some row"#, schema: "public", table: "dpd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dp WHERE a = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dp2 PARTITION OF dp FOR VALUES IN (2);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dp VALUES (2, 'two');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM dp ORDER BY a NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("dpd"), Null, T("null")],
                            &[T("dp1"), T("1"), T("one")],
                            &[T("dp2"), T("2"), T("two")],
                            &[T("dpd"), T("3"), T("three")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dpn PARTITION OF dp FOR VALUES IN (NULL);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"updated partition constraint for default partition "dpd" would be violated by some row"#, schema: "public", table: "dpd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dp WHERE a IS NULL;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dpn PARTITION OF dp FOR VALUES IN (NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rp (a INT, b INT) PARTITION BY RANGE (a, b);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rpd PARTITION OF rp DEFAULT PARTITION BY LIST (b);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rpd1 PARTITION OF rpd FOR VALUES IN (1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rpd2 PARTITION OF rpd FOR VALUES IN (2);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rp VALUES (5, 1), (15, 2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rp1 PARTITION OF rp FOR VALUES FROM (10, 0) TO (20, 0);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"updated partition constraint for default partition "rpd" would be violated by some row"#, schema: "public", table: "rpd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE rp1 PARTITION OF rp FOR VALUES FROM (0, 0) TO (5, 0);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM rp ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("rpd1"), T("5"), T("1")],
                            &[T("rpd2"), T("15"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck (a INT, b INT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ckd PARTITION OF ck (CONSTRAINT ckd_a CHECK (a > 100)) DEFAULT;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ck VALUES (101, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck1 PARTITION OF ck FOR VALUES IN (1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM ck ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("ckd"), T("101"), T("1")],
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
fn test_partition_keys_of_expressions() {
    run_scripts(&[
        ScriptTest {
            name: "partition keys of expressions route rows, prune partitions, check default partitions and attached tables, and refuse what Postgres refuses",
            set_up_script: &[
                "CREATE TABLE ke (a INT, b TEXT) PARTITION BY RANGE ((a + 1));",
                "CREATE TABLE ke1 PARTITION OF ke FOR VALUES FROM (0) TO (11);",
                "CREATE TABLE ke2 PARTITION OF ke FOR VALUES FROM (11) TO (21);",
                "INSERT INTO ke VALUES (0, 'zero'), (9, 'nine'), (10, 'ten'), (19, 'nineteen');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM ke ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("ke1"), T("0"), T("zero")],
                            &[T("ke1"), T("9"), T("nine")],
                            &[T("ke2"), T("10"), T("ten")],
                            &[T("ke2"), T("19"), T("nineteen")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ke VALUES (20, 'twenty');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "ke" found for row"#, detail: "Partition key of the failing row contains ((a + 1)) = (21).", schema: "public", table: "ke", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM ke WHERE a + 1 = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on ke1 ke"#)],
                            &[T(r#"  Filter: ((a + 1) = 5)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ke WHERE a + 1 = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM ke WHERE a + 1 > 15;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on ke2 ke"#)],
                            &[T(r#"  Filter: ((a + 1) > 15)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ke SET a = 15 WHERE a = 9;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM ke ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("ke1"), T("0"), T("zero")],
                            &[T("ke2"), T("10"), T("ten")],
                            &[T("ke2"), T("15"), T("nine")],
                            &[T("ke2"), T("19"), T("nineteen")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kl (a TEXT, b INT) PARTITION BY LIST (lower(a));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kl1 PARTITION OF kl FOR VALUES IN ('x');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kl2 PARTITION OF kl FOR VALUES IN ('y');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kl VALUES ('X', 1), ('y', 2), ('Y', 3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM kl ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", TEXT), Column("b", INT4)],
                        rows: &[
                            &[T("kl1"), T("X"), T("1")],
                            &[T("kl2"), T("y"), T("2")],
                            &[T("kl2"), T("Y"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kl VALUES ('z', 4);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "kl" found for row"#, detail: "Partition key of the failing row contains (lower(a)) = (z).", schema: "public", table: "kl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM kl WHERE lower(a) = 'y';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on kl2 kl"#)],
                            &[T(r#"  Filter: (lower(a) = 'y'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kh (a INT, b INT) PARTITION BY HASH ((a * 2), b);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kh0 PARTITION OF kh FOR VALUES WITH (MODULUS 2, REMAINDER 0);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kh1 PARTITION OF kh FOR VALUES WITH (MODULUS 2, REMAINDER 1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kh VALUES (1, 1), (2, 2), (3, 3), (4, 4);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM kh ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("kh0"), T("1"), T("1")],
                            &[T("kh1"), T("2"), T("2")],
                            &[T("kh0"), T("3"), T("3")],
                            &[T("kh0"), T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kc (a INT) PARTITION BY RANGE ((1 + 2));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "cannot use constant expression as partition key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE km (a TIMESTAMPTZ) PARTITION BY RANGE ((a + now() - now()));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: timestamp with time zone + timestamp with time zone", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kv (a INT) PARTITION BY RANGE ((a + random()::int));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "functions in partition key expression must be marked IMMUTABLE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ks (a INT) PARTITION BY RANGE ((ctid));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "partition key expressions cannot contain system column references", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ks2 (a INT) PARTITION BY RANGE ((tableoid::int + a));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "partition key expressions cannot contain system column references", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kg (a INT, g INT GENERATED ALWAYS AS (a * 2) STORED) PARTITION BY RANGE ((g + 1));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "cannot use generated column in partition key", detail: r#"Column "g" is a generated column."#, position: 87, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ku (a INT, b INT, UNIQUE (a)) PARTITION BY RANGE ((a + 0));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported UNIQUE constraint with partition key definition", detail: "UNIQUE constraints cannot be used when partition keys include expressions.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kui (a INT, b INT) PARTITION BY RANGE ((a + 0));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX kui_a ON kui (a);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported UNIQUE constraint with partition key definition", detail: "UNIQUE constraints cannot be used when partition keys include expressions.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX kui_b ON kui (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE kco (a TEXT) PARTITION BY LIST ((a COLLATE "C"));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kco1 PARTITION OF kco FOR VALUES IN ('a');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kco VALUES ('a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM kco;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", TEXT)],
                        rows: &[
                            &[T("kco1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ke DROP COLUMN a;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"cannot drop column "a" because it is part of the partition key of relation "ke""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ke ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"cannot alter column "a" because it is part of the partition key of relation "ke""#, position: 29, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kd (a INT, b INT) PARTITION BY RANGE ((a + b));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kdd PARTITION OF kd DEFAULT;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kd VALUES (1, 2), (10, 20);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kd1 PARTITION OF kd FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"updated partition constraint for default partition "kdd" would be violated by some row"#, schema: "public", table: "kdd", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM kd WHERE a = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kd1 PARTITION OF kd FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kd VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM kd ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("kd1"), T("1"), T("2")],
                            &[T("kdd"), T("10"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE kd2 (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kd2 VALUES (15, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE kd ATTACH PARTITION kd2 FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM kd2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kd2 VALUES (12, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE kd ATTACH PARTITION kd2 FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#""kd2" is already a partition"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM kd ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("kd1"), T("1"), T("2")],
                            &[T("kdd"), T("10"), T("20")],
                            &[T("kd2"), T("12"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ke;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_partition_key_expressions_on_branches() {
    run_scripts(&[
        ScriptTest {
            name: "each branch folds a partition key expression by its own definition of the function the key calls",
            set_up_script: &[
                "CREATE FUNCTION kf(int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT $1 * 2';",
                "CREATE TABLE br (a int) PARTITION BY RANGE (kf(a));",
                "CREATE TABLE br1 PARTITION OF br FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE br2 PARTITION OF br FOR VALUES FROM (10) TO (100);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'key');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION kf(int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT $1 * 20';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO br VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM br;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("br2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO br VALUES (5);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "br" found for row"#, detail: "Partition key of the failing row contains (kf(a)) = (100).", schema: "public", table: "br", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO br VALUES (1), (5);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM br ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("br1"), T("1")],
                            &[T("br2"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM br WHERE a * 2 = 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on br2 br"#)],
                            &[T(r#"  Filter: ((a * 2) = 10)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM br;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("br2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM br WHERE a * 20 = 20;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Seq Scan on br2 br"#)],
                            &[T(r#"  Filter: ((a * 20) = 20)"#)],
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
fn test_partitionwise_joins() {
    run_scripts(&[
        ScriptTest {
            name: "partitionwise joins match the partitions of their inputs",
            set_up_script: &[
                "SET enable_partitionwise_join TO true;",
                "CREATE TABLE prt1 (a INT, b INT, c VARCHAR) PARTITION BY RANGE (a);",
                "CREATE TABLE prt1_p1 PARTITION OF prt1 FOR VALUES FROM (0) TO (250);",
                "CREATE TABLE prt1_p3 PARTITION OF prt1 FOR VALUES FROM (500) TO (600);",
                "CREATE TABLE prt1_p2 PARTITION OF prt1 FOR VALUES FROM (250) TO (500);",
                "INSERT INTO prt1 SELECT i, i % 25, to_char(i, 'FM0000') FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x), (VALUES (0), (60), (120), (180), (240), (300), (360), (420), (480), (540)) t(y), LATERAL (SELECT x * 6 + y) s(i) WHERE i % 2 = 0;",
                "CREATE INDEX iprt1_p1_a ON prt1_p1 (a);",
                "CREATE INDEX iprt1_p2_a ON prt1_p2 (a);",
                "CREATE INDEX iprt1_p3_a ON prt1_p3 (a);",
                "ANALYZE prt1;",
                "CREATE TABLE prt2 (a INT, b INT, c VARCHAR) PARTITION BY RANGE (b);",
                "CREATE TABLE prt2_p1 PARTITION OF prt2 FOR VALUES FROM (0) TO (250);",
                "CREATE TABLE prt2_p2 PARTITION OF prt2 FOR VALUES FROM (250) TO (500);",
                "CREATE TABLE prt2_p3 PARTITION OF prt2 FOR VALUES FROM (500) TO (600);",
                "INSERT INTO prt2 SELECT i % 25, i, to_char(i, 'FM0000') FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x), (VALUES (0), (60), (120), (180), (240), (300), (360), (420), (480), (540)) t(y), LATERAL (SELECT x * 6 + y) s(i) WHERE i % 3 = 0;",
                "CREATE INDEX iprt2_p1_b ON prt2_p1 (b);",
                "CREATE INDEX iprt2_p2_b ON prt2_p2 (b);",
                "CREATE INDEX iprt2_p3_b ON prt2_p3 (b);",
                "ANALYZE prt2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Join"#)],
                            &[T(r#"              Hash Cond: (t2_1.b = t1_1.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p1 t2_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p1 t1_1"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Join"#)],
                            &[T(r#"              Hash Cond: (t2_2.b = t1_2.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p2 t2_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p2 t1_2"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Join"#)],
                            &[T(r#"              Hash Cond: (t2_3.b = t1_3.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p3 t2_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p3 t1_3"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("0"), T("0000"), T("0"), T("0000")],
                            &[T("150"), T("0150"), T("150"), T("0150")],
                            &[T("300"), T("0300"), T("300"), T("0300")],
                            &[T("450"), T("0450"), T("450"), T("0450")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM prt1 t1 LEFT JOIN prt1 t2 ON t1.a = t2.a LEFT JOIN prt1 t3 ON t2.a = t3.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Left Join"#)],
                            &[T(r#"              Hash Cond: (t2_1.a = t3_1.a)"#)],
                            &[T(r#"              ->  Hash Left Join"#)],
                            &[T(r#"                    Hash Cond: (t1_1.a = t2_1.a)"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p1 t1_1"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on prt1_p1 t2_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p1 t3_1"#)],
                            &[T(r#"        ->  Hash Left Join"#)],
                            &[T(r#"              Hash Cond: (t2_2.a = t3_2.a)"#)],
                            &[T(r#"              ->  Hash Left Join"#)],
                            &[T(r#"                    Hash Cond: (t1_2.a = t2_2.a)"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p2 t1_2"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on prt1_p2 t2_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p2 t3_2"#)],
                            &[T(r#"        ->  Hash Left Join"#)],
                            &[T(r#"              Hash Cond: (t2_3.a = t3_3.a)"#)],
                            &[T(r#"              ->  Hash Left Join"#)],
                            &[T(r#"                    Hash Cond: (t1_3.a = t2_3.a)"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p3 t1_3"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on prt1_p3 t2_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p3 t3_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM prt1 t1 LEFT JOIN prt1 t2 ON t1.a = t2.a LEFT JOIN prt1 t3 ON t2.a = t3.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a, t2.b"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_1.b = t1_1.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p1 t2_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p1 t1_1"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_2.b = t1_2.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p2 t2_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p2 t1_2"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_3.b = t1_3.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p3 t2_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p3 t1_3"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("0"), T("0000"), T("0"), T("0000")],
                            &[T("150"), T("0150"), T("150"), T("0150")],
                            &[T("300"), T("0300"), T("300"), T("0300")],
                            &[T("450"), T("0450"), T("450"), T("0450")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 RIGHT JOIN prt2 t2 ON t1.a = t2.b WHERE t2.a = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a, t2.b"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t1_1.a = t2_1.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p1 t1_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p1 t2_1"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t1_2.a = t2_2.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p2 t1_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p2 t2_2"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t1_3.a = t2_3.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p3 t1_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p3 t2_3"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 RIGHT JOIN prt2 t2 ON t1.a = t2.b WHERE t2.a = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("0"), T("0000"), T("0"), T("0000")],
                            &[T("150"), T("0150"), T("150"), T("0150")],
                            &[T("300"), T("0300"), T("300"), T("0300")],
                            &[T("450"), T("0450"), T("450"), T("0450")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE b = 0) t1 FULL JOIN (SELECT * FROM prt2 WHERE a = 0) t2 ON t1.a = t2.b ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: prt1.a, prt2.b"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Full Join"#)],
                            &[T(r#"              Hash Cond: (prt1_1.a = prt2_1.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p1 prt1_1"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p1 prt2_1"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                            &[T(r#"        ->  Hash Full Join"#)],
                            &[T(r#"              Hash Cond: (prt1_2.a = prt2_2.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p2 prt1_2"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p2 prt2_2"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                            &[T(r#"        ->  Hash Full Join"#)],
                            &[T(r#"              Hash Cond: (prt1_3.a = prt2_3.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p3 prt1_3"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p3 prt2_3"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE b = 0) t1 FULL JOIN (SELECT * FROM prt2 WHERE a = 0) t2 ON t1.a = t2.b ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("0"), T("0000"), T("0"), T("0000")],
                            &[T("150"), T("0150"), T("150"), T("0150")],
                            &[T("300"), T("0300"), T("300"), T("0300")],
                            &[T("450"), T("0450"), T("450"), T("0450")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t2.b FROM prt2 t2 WHERE t2.a = 0) AND t1.b = 0 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Semi Join"#)],
                            &[T(r#"              Hash Cond: (t1_1.a = t2_1.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p1 t1_1"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p1 t2_1"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                            &[T(r#"        ->  Hash Semi Join"#)],
                            &[T(r#"              Hash Cond: (t1_2.a = t2_2.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p2 t1_2"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p2 t2_2"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                            &[T(r#"        ->  Nested Loop Semi Join"#)],
                            &[T(r#"              Join Filter: (t1_3.a = t2_3.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p3 t1_3"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p3 t2_3"#)],
                            &[T(r#"                    Filter: (a = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t2.b FROM prt2 t2 WHERE t2.a = 0) AND t1.b = 0 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("0"), T("0"), T("0000")],
                            &[T("150"), T("0"), T("0150")],
                            &[T("300"), T("0"), T("0300")],
                            &[T("450"), T("0"), T("0450")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.* FROM prt1 t1 WHERE NOT EXISTS (SELECT 1 FROM prt2 t2 WHERE t1.a = t2.b) AND t1.b = 0 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Right Anti Join"#)],
                            &[T(r#"              Hash Cond: (t2_1.b = t1_1.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p1 t2_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p1 t1_1"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Right Anti Join"#)],
                            &[T(r#"              Hash Cond: (t2_2.b = t1_2.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p2 t2_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p2 t1_2"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Right Anti Join"#)],
                            &[T(r#"              Hash Cond: (t2_3.b = t1_3.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p3 t2_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p3 t1_3"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("our storage counts every row visible (allvisfrac 1), so index-only scans cost less than in Postgres and win this partition's join"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.* FROM prt1 t1 WHERE NOT EXISTS (SELECT 1 FROM prt2 t2 WHERE t1.a = t2.b) AND t1.b = 0 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.a < 450 AND t2.b > 250 AND t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a"#)],
                            &[T(r#"  ->  Hash Join"#)],
                            &[T(r#"        Hash Cond: (t2.b = t1.a)"#)],
                            &[T(r#"        ->  Seq Scan on prt2_p2 t2"#)],
                            &[T(r#"              Filter: (b > 250)"#)],
                            &[T(r#"        ->  Hash"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p2 t1"#)],
                            &[T(r#"                    Filter: ((a < 450) AND (b = 0))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.a < 450 AND t2.b > 250 AND t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("300"), T("0300"), T("300"), T("0300")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: prt1.a, prt2.b"#)],
                            &[T(r#"  ->  Hash Right Join"#)],
                            &[T(r#"        Hash Cond: (prt2.b = prt1.a)"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p2 prt2_1"#)],
                            &[T(r#"                    Filter: (b > 250)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p3 prt2_2"#)],
                            &[T(r#"                    Filter: (b > 250)"#)],
                            &[T(r#"        ->  Hash"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p1 prt1_1"#)],
                            &[T(r#"                          Filter: ((a < 450) AND (b = 0))"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p2 prt1_2"#)],
                            &[T(r#"                          Filter: ((a < 450) AND (b = 0))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("0"), T("0000"), Null, Null],
                            &[T("150"), T("0150"), Null, Null],
                            &[T("300"), T("0300"), T("300"), T("0300")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM prt1 t1 LEFT JOIN LATERAL (SELECT t2.a AS t2a, t3.a AS t3a, least(t1.a, t2.a, t3.b) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss ON t1.a = ss.t2a WHERE t1.b = 0 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Nested Loop Left Join"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p1 t1_1"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t3_1.b = t2_1.a)"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p1 t3_1"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on prt1_p1 t2_1"#)],
                            &[T(r#"                                Filter: (t1_1.a = a)"#)],
                            &[T(r#"        ->  Nested Loop Left Join"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p2 t1_2"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t3_2.b = t2_2.a)"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p2 t3_2"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on prt1_p2 t2_2"#)],
                            &[T(r#"                                Filter: (t1_2.a = a)"#)],
                            &[T(r#"        ->  Nested Loop Left Join"#)],
                            &[T(r#"              ->  Seq Scan on prt1_p3 t1_3"#)],
                            &[T(r#"                    Filter: (b = 0)"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t3_3.b = t2_3.a)"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_p3 t3_3"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on prt1_p3 t2_3"#)],
                            &[T(r#"                                Filter: (t1_3.a = a)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("our storage counts every row visible (allvisfrac 1), so index-only scans cost less than in Postgres and win this partition's join"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prt1 t1 LEFT JOIN LATERAL (SELECT t2.a AS t2a, t3.a AS t3a, least(t1.a, t2.a, t3.b) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss ON t1.a = ss.t2a WHERE t1.b = 0 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", VARCHAR), Column("t2a", INT4), Column("t3a", INT4), Column("least", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0000"), T("0"), T("0"), T("0")],
                            &[T("150"), T("0"), T("0150"), T("150"), T("0"), T("150")],
                            &[T("300"), T("0"), T("0300"), T("300"), T("0"), T("300")],
                            &[T("450"), T("0"), T("0450"), T("450"), T("0"), T("450")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plt1 (a INT, b INT, c VARCHAR) PARTITION BY LIST (c);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plt1_p1 PARTITION OF plt1 FOR VALUES IN ('0000', '0003', '0004', '0010');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plt1_p2 PARTITION OF plt1 FOR VALUES IN ('0001', '0005', '0002', '0009');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plt1_p3 PARTITION OF plt1 FOR VALUES IN ('0006', '0007', '0008', '0011');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO plt1 SELECT i, i, to_char(i / 50, 'FM0000') FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x), (VALUES (0), (60), (120), (180), (240), (300), (360), (420), (480), (540)) t(y), LATERAL (SELECT x * 6 + y) s(i) WHERE i % 2 = 0;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE plt1;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plt2 (a INT, b INT, c VARCHAR) PARTITION BY LIST (c);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plt2_p1 PARTITION OF plt2 FOR VALUES IN ('0000', '0003', '0004', '0010');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plt2_p2 PARTITION OF plt2 FOR VALUES IN ('0001', '0005', '0002', '0009');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plt2_p3 PARTITION OF plt2 FOR VALUES IN ('0006', '0007', '0008', '0011');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO plt2 SELECT i, i, to_char(i / 50, 'FM0000') FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x), (VALUES (0), (60), (120), (180), (240), (300), (360), (420), (480), (540)) t(y), LATERAL (SELECT x * 6 + y) s(i) WHERE i % 3 = 0;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE plt2;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.a, t2.c FROM plt1 t1 LEFT JOIN plt2 t2 ON t1.a = t2.a AND t1.c = t2.c WHERE t1.b < 100 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: ((t2_1.a = t1_1.a) AND ((t2_1.c)::text = (t1_1.c)::text))"#)],
                            &[T(r#"              ->  Seq Scan on plt2_p1 t2_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on plt1_p1 t1_1"#)],
                            &[T(r#"                          Filter: (b < 100)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: ((t2_2.a = t1_2.a) AND ((t2_2.c)::text = (t1_2.c)::text))"#)],
                            &[T(r#"              ->  Seq Scan on plt2_p2 t2_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on plt1_p2 t1_2"#)],
                            &[T(r#"                          Filter: (b < 100)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: ((t2_3.a = t1_3.a) AND ((t2_3.c)::text = (t1_3.c)::text))"#)],
                            &[T(r#"              ->  Seq Scan on plt2_p3 t2_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on plt1_p3 t1_3"#)],
                            &[T(r#"                          Filter: (b < 100)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.a, t2.c FROM plt1 t1 LEFT JOIN plt2 t2 ON t1.a = t2.a AND t1.c = t2.c WHERE t1.b < 100 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("a", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("0"), T("0000"), T("0"), T("0000")],
                            &[T("6"), T("0000"), T("6"), T("0000")],
                            &[T("12"), T("0000"), T("12"), T("0000")],
                            &[T("18"), T("0000"), T("18"), T("0000")],
                            &[T("24"), T("0000"), T("24"), T("0000")],
                            &[T("30"), T("0000"), T("30"), T("0000")],
                            &[T("36"), T("0000"), T("36"), T("0000")],
                            &[T("42"), T("0000"), T("42"), T("0000")],
                            &[T("48"), T("0000"), T("48"), T("0000")],
                            &[T("54"), T("0001"), T("54"), T("0001")],
                            &[T("60"), T("0001"), T("60"), T("0001")],
                            &[T("66"), T("0001"), T("66"), T("0001")],
                            &[T("72"), T("0001"), T("72"), T("0001")],
                            &[T("78"), T("0001"), T("78"), T("0001")],
                            &[T("84"), T("0001"), T("84"), T("0001")],
                            &[T("90"), T("0001"), T("90"), T("0001")],
                            &[T("96"), T("0001"), T("96"), T("0001")],
                        ],
                        tag: "SELECT 17",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt1_adv (a INT, b INT, c VARCHAR) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt1_adv_p1 PARTITION OF prt1_adv FOR VALUES FROM (100) TO (200);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt1_adv_p2 PARTITION OF prt1_adv FOR VALUES FROM (200) TO (300);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt1_adv_p3 PARTITION OF prt1_adv FOR VALUES FROM (300) TO (400);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO prt1_adv SELECT i, i % 25, to_char(i, 'FM0000') FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x), (VALUES (100), (130), (160), (190), (220), (250), (280), (310), (340), (370)) t(y), LATERAL (SELECT x * 3 + y) s(i);",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE prt1_adv;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt2_adv (a INT, b INT, c VARCHAR) PARTITION BY RANGE (b);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt2_adv_p1 PARTITION OF prt2_adv FOR VALUES FROM (100) TO (150);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt2_adv_p2 PARTITION OF prt2_adv FOR VALUES FROM (200) TO (300);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt2_adv_p3 PARTITION OF prt2_adv FOR VALUES FROM (350) TO (500);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO prt2_adv SELECT i % 25, i, to_char(i, 'FM0000') FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x), (VALUES (100), (120), (140), (200), (230), (260), (290), (350), (400), (450)) t(y), LATERAL (SELECT x * 2 + y) s(i) WHERE i < 150 OR i >= 200 AND i < 300 OR i >= 350;",
                    expected: Expected::Tag("INSERT 0 90"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE prt2_adv;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.a, t2.c FROM prt1_adv t1 INNER JOIN prt2_adv t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Join"#)],
                            &[T(r#"              Hash Cond: (t2_1.b = t1_1.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_adv_p1 t2_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_adv_p1 t1_1"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Join"#)],
                            &[T(r#"              Hash Cond: (t2_2.b = t1_2.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_adv_p2 t2_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_adv_p2 t1_2"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Join"#)],
                            &[T(r#"              Hash Cond: (t2_3.b = t1_3.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_adv_p3 t2_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_adv_p3 t1_3"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.a, t2.c FROM prt1_adv t1 INNER JOIN prt2_adv t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("a", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("100"), T("0100"), T("0"), T("0100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.a, t2.c FROM prt1_adv t1 LEFT JOIN prt2_adv t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a, t2.b"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_1.b = t1_1.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_adv_p1 t2_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_adv_p1 t1_1"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_2.b = t1_2.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_adv_p2 t2_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_adv_p2 t1_2"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_3.b = t1_3.a)"#)],
                            &[T(r#"              ->  Seq Scan on prt2_adv_p3 t2_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_adv_p3 t1_3"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.c, t2.a, t2.c FROM prt1_adv t1 LEFT JOIN prt2_adv t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR), Column("a", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("100"), T("0100"), T("0"), T("0100")],
                            &[T("175"), T("0175"), Null, Null],
                            &[T("250"), T("0250"), Null, Null],
                            &[T("325"), T("0325"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.c, t2.a, t2.c FROM prt2_adv t1 LEFT JOIN prt1_adv t2 ON (t1.b = t2.a) WHERE t1.a = 0 ORDER BY t1.b, t2.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.b, t2.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_1.a = t1_1.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_adv_p1 t2_1"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_adv_p1 t1_1"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_2.a = t1_2.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_adv_p2 t2_2"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_adv_p2 t1_2"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                            &[T(r#"        ->  Hash Right Join"#)],
                            &[T(r#"              Hash Cond: (t2_3.a = t1_3.b)"#)],
                            &[T(r#"              ->  Seq Scan on prt1_adv_p3 t2_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_adv_p3 t1_3"#)],
                            &[T(r#"                          Filter: (a = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt1_m (a INT, b INT, c INT) PARTITION BY RANGE (a, ((a + b) / 2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt1_m_p1 PARTITION OF prt1_m FOR VALUES FROM (0, 0) TO (250, 250);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt1_m_p2 PARTITION OF prt1_m FOR VALUES FROM (250, 250) TO (500, 500);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt1_m_p3 PARTITION OF prt1_m FOR VALUES FROM (500, 500) TO (600, 600);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO prt1_m SELECT i, i, i % 25 FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x), (VALUES (0), (60), (120), (180), (240), (300), (360), (420), (480), (540)) t(y), LATERAL (SELECT x * 6 + y) s(i) WHERE i % 2 = 0;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE prt1_m;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt2_m (a INT, b INT, c INT) PARTITION BY RANGE (((b + a) / 2), b);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt2_m_p1 PARTITION OF prt2_m FOR VALUES FROM (0, 0) TO (250, 250);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt2_m_p2 PARTITION OF prt2_m FOR VALUES FROM (250, 250) TO (500, 500);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE prt2_m_p3 PARTITION OF prt2_m FOR VALUES FROM (500, 500) TO (600, 600);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO prt2_m SELECT i, i, i % 25 FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d(x), (VALUES (0), (60), (120), (180), (240), (300), (360), (420), (480), (540)) t(y), LATERAL (SELECT x * 6 + y) s(i) WHERE i % 3 = 0;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE prt2_m;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t1.b, t2.b, t2.a FROM (SELECT * FROM prt1_m WHERE prt1_m.c = 0) t1 FULL JOIN (SELECT * FROM prt2_m WHERE prt2_m.c = 0) t2 ON (t1.a = (t2.b + t2.a) / 2 AND t2.b = (t1.a + t1.b) / 2) ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: prt1_m.a, prt2_m.b"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Hash Full Join"#)],
                            &[T(r#"              Hash Cond: ((prt1_m_1.a = ((prt2_m_1.b + prt2_m_1.a) / 2)) AND (((prt1_m_1.a + prt1_m_1.b) / 2) = prt2_m_1.b))"#)],
                            &[T(r#"              ->  Seq Scan on prt1_m_p1 prt1_m_1"#)],
                            &[T(r#"                    Filter: (c = 0)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_m_p1 prt2_m_1"#)],
                            &[T(r#"                          Filter: (c = 0)"#)],
                            &[T(r#"        ->  Hash Full Join"#)],
                            &[T(r#"              Hash Cond: ((prt1_m_2.a = ((prt2_m_2.b + prt2_m_2.a) / 2)) AND (((prt1_m_2.a + prt1_m_2.b) / 2) = prt2_m_2.b))"#)],
                            &[T(r#"              ->  Seq Scan on prt1_m_p2 prt1_m_2"#)],
                            &[T(r#"                    Filter: (c = 0)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_m_p2 prt2_m_2"#)],
                            &[T(r#"                          Filter: (c = 0)"#)],
                            &[T(r#"        ->  Hash Full Join"#)],
                            &[T(r#"              Hash Cond: ((prt1_m_3.a = ((prt2_m_3.b + prt2_m_3.a) / 2)) AND (((prt1_m_3.a + prt1_m_3.b) / 2) = prt2_m_3.b))"#)],
                            &[T(r#"              ->  Seq Scan on prt1_m_p3 prt1_m_3"#)],
                            &[T(r#"                    Filter: (c = 0)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Seq Scan on prt2_m_p3 prt2_m_3"#)],
                            &[T(r#"                          Filter: (c = 0)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.a, t1.b, t2.b, t2.a FROM (SELECT * FROM prt1_m WHERE prt1_m.c = 0) t1 FULL JOIN (SELECT * FROM prt2_m WHERE prt2_m.c = 0) t2 ON (t1.a = (t2.b + t2.a) / 2 AND t2.b = (t1.a + t1.b) / 2) ORDER BY t1.a, t2.b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("b", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0")],
                            &[T("150"), T("150"), T("150"), T("150")],
                            &[T("300"), T("300"), T("300"), T("300")],
                            &[T("450"), T("450"), T("450"), T("450")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_partitionwise_join TO false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.a, t2.b FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.a"#)],
                            &[T(r#"  ->  Hash Join"#)],
                            &[T(r#"        Hash Cond: (t2.b = t1.a)"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p1 t2_1"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p2 t2_2"#)],
                            &[T(r#"              ->  Seq Scan on prt2_p3 t2_3"#)],
                            &[T(r#"        ->  Hash"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p1 t1_1"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p2 t1_2"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
                            &[T(r#"                    ->  Seq Scan on prt1_p3 t1_3"#)],
                            &[T(r#"                          Filter: (b = 0)"#)],
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
fn test_identical_dependencies_of_one_command() {
    run_scripts(&[
        ScriptTest {
            name: "a command stores the identical dependencies it records twice, as Postgres does",
            set_up_script: &[
                "CREATE TABLE pm (a INT, b INT) PARTITION BY RANGE (a, ((a + b) / 2));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT objsubid, refobjsubid, deptype FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'pm'::regclass AND refobjid = 'pm'::regclass ORDER BY objsubid, refobjsubid;",
                    expected: Expected::Rows {
                        columns: &[Column("objsubid", INT4), Column("refobjsubid", INT4), Column("deptype", CHAR)],
                        rows: &[
                            &[T("1"), T("0"), T("i")],
                            &[T("1"), T("0"), T("i")],
                            &[T("2"), T("0"), T("i")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ix (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ix_a_expr ON ix (a, (a + 1));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objsubid, refobjsubid, deptype FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'ix_a_expr'::regclass AND refobjid = 'ix'::regclass ORDER BY objsubid, refobjsubid;",
                    expected: Expected::Rows {
                        columns: &[Column("objsubid", INT4), Column("refobjsubid", INT4), Column("deptype", CHAR)],
                        rows: &[
                            &[T("0"), T("1"), T("a")],
                            &[T("0"), T("1"), T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE pm;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ix;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.objid >= 16384 AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = d.objid);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replaying a command on another branch derives the same ids for the identical dependencies it records",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pm (a INT, b INT) PARTITION BY RANGE (a, ((a + b) / 2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'pm on other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pm (a INT, b INT) PARTITION BY RANGE (a, ((a + b) / 2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'pm on main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objsubid, refobjsubid, deptype FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'pm'::regclass AND refobjid = 'pm'::regclass ORDER BY objsubid, refobjsubid;",
                    expected: Expected::Rows {
                        columns: &[Column("objsubid", INT4), Column("refobjsubid", INT4), Column("deptype", CHAR)],
                        rows: &[
                            &[T("1"), T("0"), T("i")],
                            &[T("1"), T("0"), T("i")],
                            &[T("2"), T("0"), T("i")],
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
fn test_partitionwise_join_parameterized_inner_scans() {
    run_scripts(&[
        ScriptTest {
            name: "a parameterized inner scan of a partitionwise join drops the conditions its index conditions imply",
            set_up_script: &[
                "CREATE TABLE fract_t (id BIGINT, PRIMARY KEY (id)) PARTITION BY RANGE (id);",
                "CREATE TABLE fract_t0 PARTITION OF fract_t FOR VALUES FROM ('0') TO ('1000');",
                "CREATE TABLE fract_t1 PARTITION OF fract_t FOR VALUES FROM ('1000') TO ('2000');",
                "INSERT INTO fract_t (id) SELECT a + 10 * b + 100 * c FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) x(a), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) y(b), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9), (10), (11), (12), (13), (14), (15), (16), (17), (18), (19)) z(c);",
                "ANALYZE fract_t;",
                "SET max_parallel_workers_per_gather = 0;",
                "SET enable_partitionwise_join = on;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT x.id, y.id FROM fract_t x LEFT JOIN fract_t y USING (id) ORDER BY x.id ASC LIMIT 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Merge Append"#)],
                            &[T(r#"        Sort Key: x.id"#)],
                            &[T(r#"        ->  Merge Left Join"#)],
                            &[T(r#"              Merge Cond: (x_1.id = y_1.id)"#)],
                            &[T(r#"              ->  Index Only Scan using fract_t0_pkey on fract_t0 x_1"#)],
                            &[T(r#"              ->  Index Only Scan using fract_t0_pkey on fract_t0 y_1"#)],
                            &[T(r#"        ->  Merge Left Join"#)],
                            &[T(r#"              Merge Cond: (x_2.id = y_2.id)"#)],
                            &[T(r#"              ->  Index Only Scan using fract_t1_pkey on fract_t1 x_2"#)],
                            &[T(r#"              ->  Index Only Scan using fract_t1_pkey on fract_t1 y_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT x.id, y.id FROM fract_t x LEFT JOIN fract_t y USING (id) ORDER BY x.id DESC LIMIT 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Merge Append"#)],
                            &[T(r#"        Sort Key: x.id DESC"#)],
                            &[T(r#"        ->  Nested Loop Left Join"#)],
                            &[T(r#"              ->  Index Only Scan Backward using fract_t0_pkey on fract_t0 x_1"#)],
                            &[T(r#"              ->  Index Only Scan using fract_t0_pkey on fract_t0 y_1"#)],
                            &[T(r#"                    Index Cond: (id = x_1.id)"#)],
                            &[T(r#"        ->  Nested Loop Left Join"#)],
                            &[T(r#"              ->  Index Only Scan Backward using fract_t1_pkey on fract_t1 x_2"#)],
                            &[T(r#"              ->  Index Only Scan using fract_t1_pkey on fract_t1 y_2"#)],
                            &[T(r#"                    Index Cond: (id = x_2.id)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT x.id, y.id FROM fract_t x LEFT JOIN fract_t y USING (id) ORDER BY x.id DESC LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Merge Append"#)],
                            &[T(r#"        Sort Key: x.id DESC"#)],
                            &[T(r#"        ->  Nested Loop Left Join"#)],
                            &[T(r#"              ->  Index Only Scan Backward using fract_t0_pkey on fract_t0 x_1"#)],
                            &[T(r#"              ->  Index Only Scan using fract_t0_pkey on fract_t0 y_1"#)],
                            &[T(r#"                    Index Cond: (id = x_1.id)"#)],
                            &[T(r#"        ->  Nested Loop Left Join"#)],
                            &[T(r#"              ->  Index Only Scan Backward using fract_t1_pkey on fract_t1 x_2"#)],
                            &[T(r#"              ->  Index Only Scan using fract_t1_pkey on fract_t1 y_2"#)],
                            &[T(r#"                    Index Cond: (id = x_2.id)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x.id, y.id FROM fract_t x LEFT JOIN fract_t y USING (id) ORDER BY x.id DESC LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("id", INT8)],
                        rows: &[
                            &[T("1999"), T("1999")],
                            &[T("1998"), T("1998")],
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
fn test_partitionwise_aggregates() {
    run_scripts(&[
        ScriptTest {
            name: "partial aggregates of each partition combine into the aggregates of the whole",
            set_up_script: &[
                "SET enable_partitionwise_aggregate TO true;",
                "SET enable_incremental_sort TO off;",
                "CREATE TABLE pa (a INT, z INT, b INT, c NUMERIC, d TEXT, e INTERVAL, f FLOAT8, g BIGINT, h INT[], i SMALLINT, j BYTEA, k BOOL, m FLOAT4, n MONEY, PRIMARY KEY (a, z)) PARTITION BY RANGE (a);",
                "CREATE TABLE pa_p1 PARTITION OF pa FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pa_p2 PARTITION OF pa FOR VALUES FROM (10) TO (20);",
                "CREATE TABLE pa_p3 PARTITION OF pa FOR VALUES FROM (20) TO (30);",
                r#"INSERT INTO pa SELECT x % 30, x, x % 4, x * 1.5, 'v' || x, make_interval(days => x % 100), x / 3.0, x * 1000000000000, ARRAY[x, x + 1], (x % 1000)::SMALLINT, ('\x0' || (x % 10))::BYTEA, x % 3 = 0, x / 7.0, x::MONEY FROM (SELECT p + 10 * q + 100 * r AS x FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) p(p), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) q(q), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9), (10), (11), (12), (13), (14), (15), (16), (17), (18), (19), (20), (21), (22), (23), (24), (25), (26), (27), (28), (29)) r(r)) s ORDER BY s.x % 30, s.x;"#,
                "INSERT INTO pa VALUES (29, 3000, 5, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);",
                "ANALYZE pa;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, count(*), sum(a), avg(a), sum(g), avg(g), sum(c), avg(c) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(*), count(c), sum(a), avg(a), sum(g), avg(g), sum(c), avg(c), sum(i), avg(i) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8), Column("count", INT8), Column("sum", INT8), Column("avg", NUMERIC), Column("sum", NUMERIC), Column("avg", NUMERIC), Column("sum", NUMERIC), Column("avg", NUMERIC), Column("sum", INT8), Column("avg", NUMERIC)],
                        rows: &[
                            &[T("0"), T("750"), T("750"), T("10500"), T("14.0000000000000000"), T("1123500000000000000"), T("1498000000000000.0000"), T("1685250.0"), T("2247.0000000000000000"), T("373500"), T("498.0000000000000000")],
                            &[T("1"), T("750"), T("750"), T("11250"), T("15.0000000000000000"), T("1124250000000000000"), T("1499000000000000.0000"), T("1686375.0"), T("2248.5000000000000000"), T("374250"), T("499.0000000000000000")],
                            &[T("2"), T("750"), T("750"), T("10500"), T("14.0000000000000000"), T("1125000000000000000"), T("1500000000000000.0000"), T("1687500.0"), T("2250.0000000000000000"), T("375000"), T("500.0000000000000000")],
                            &[T("3"), T("750"), T("750"), T("11250"), T("15.0000000000000000"), T("1125750000000000000"), T("1501000000000000.0000"), T("1688625.0"), T("2251.5000000000000000"), T("375750"), T("501.0000000000000000")],
                            &[T("5"), T("1"), T("0"), T("29"), T("29.0000000000000000"), Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, variance(a), stddev(a), var_pop(i), stddev_samp(g), var_samp(c), stddev_pop(c), variance(g) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, variance(a), stddev(a), var_pop(i), stddev_samp(g), var_samp(c), stddev_pop(c), variance(g) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("variance", NUMERIC), Column("stddev", NUMERIC), Column("var_pop", NUMERIC), Column("stddev_samp", NUMERIC), Column("var_samp", NUMERIC), Column("stddev_pop", NUMERIC), Column("variance", NUMERIC)],
                        rows: &[
                            &[T("0"), T("74.7663551401869159"), T("8.6467540233423384"), T("83332.000000000000"), T("866602561731732"), T("1689750.000000000000"), T("1299.036950975606"), T("751000000000000000000000000000")],
                            &[T("1"), T("74.7663551401869159"), T("8.6467540233423384"), T("83332.000000000000"), T("866602561731732"), T("1689750.000000000000"), T("1299.036950975606"), T("751000000000000000000000000000")],
                            &[T("2"), T("74.7663551401869159"), T("8.6467540233423384"), T("83332.000000000000"), T("866602561731732"), T("1689750.000000000000"), T("1299.036950975606"), T("751000000000000000000000000000")],
                            &[T("3"), T("74.7663551401869159"), T("8.6467540233423384"), T("83332.000000000000"), T("866602561731732"), T("1689750.000000000000"), T("1299.036950975606"), T("751000000000000000000000000000")],
                            &[T("5"), Null, Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, avg(f), variance(f), stddev(m), sum(f), sum(m), avg(m) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, round(avg(f)::NUMERIC, 6), round(variance(f)::NUMERIC, 6), round(stddev(m)::NUMERIC, 3), round(sum(f)::NUMERIC, 6), round(sum(m)::NUMERIC, 0), round(avg(m)::NUMERIC, 3) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC)],
                        rows: &[
                            &[T("0"), T("499.333333"), T("83444.444444"), T("123.800"), T("374500.000000"), T("160500"), T("214.000")],
                            &[T("1"), T("499.666667"), T("83444.444444"), T("123.800"), T("374750.000000"), T("160607"), T("214.143")],
                            &[T("2"), T("500.000000"), T("83444.444444"), T("123.800"), T("375000.000000"), T("160714"), T("214.286")],
                            &[T("3"), T("500.333333"), T("83444.444444"), T("123.800"), T("375250.000000"), T("160821"), T("214.429")],
                            &[T("5"), Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, regr_count(f, a), round(regr_slope(f, a)::NUMERIC, 6), round(regr_intercept(f, a)::NUMERIC, 6), round(corr(f, a)::NUMERIC, 6), round(covar_pop(f, a)::NUMERIC, 6), round(covar_samp(f, a)::NUMERIC, 6), round(regr_r2(f, a)::NUMERIC, 6), round(regr_avgx(f, a)::NUMERIC, 6), round(regr_avgy(f, a)::NUMERIC, 6), round(regr_sxx(f, a)::NUMERIC, 6), round(regr_syy(f, a)::NUMERIC, 6), round(regr_sxy(f, a)::NUMERIC, 6) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, regr_count(f, a), round(regr_slope(f, a)::NUMERIC, 6), round(regr_intercept(f, a)::NUMERIC, 6), round(corr(f, a)::NUMERIC, 6), round(covar_pop(f, a)::NUMERIC, 6), round(covar_samp(f, a)::NUMERIC, 6), round(regr_r2(f, a)::NUMERIC, 6), round(regr_avgx(f, a)::NUMERIC, 6), round(regr_avgy(f, a)::NUMERIC, 6), round(regr_sxx(f, a)::NUMERIC, 6), round(regr_syy(f, a)::NUMERIC, 6), round(regr_sxy(f, a)::NUMERIC, 6) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("regr_count", INT8), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC), Column("round", NUMERIC)],
                        rows: &[
                            &[T("0"), T("750"), T("0.333333"), T("494.666667"), T("0.009978"), T("24.888889"), T("24.922118"), T("0.000100"), T("14.000000"), T("499.333333"), T("56000.000000"), T("62499888.888889"), T("18666.666667")],
                            &[T("1"), T("750"), T("0.333333"), T("494.666667"), T("0.009978"), T("24.888889"), T("24.922118"), T("0.000100"), T("15.000000"), T("499.666667"), T("56000.000000"), T("62499888.888889"), T("18666.666667")],
                            &[T("2"), T("750"), T("0.333333"), T("495.333333"), T("0.009978"), T("24.888889"), T("24.922118"), T("0.000100"), T("14.000000"), T("500.000000"), T("56000.000000"), T("62499888.888889"), T("18666.666667")],
                            &[T("3"), T("750"), T("0.333333"), T("495.333333"), T("0.009978"), T("24.888889"), T("24.922118"), T("0.000100"), T("15.000000"), T("500.333333"), T("56000.000000"), T("62499888.888889"), T("18666.666667")],
                            &[T("5"), T("0"), Null, Null, Null, Null, Null, Null, Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, sum(e), avg(e), min(e), max(e) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, sum(e), avg(e), min(e), max(e) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("sum", INTERVAL), Column("avg", INTERVAL), Column("min", INTERVAL), Column("max", INTERVAL)],
                        rows: &[
                            &[T("0"), T("36000 days"), T("48 days"), T("00:00:00"), T("96 days")],
                            &[T("1"), T("36750 days"), T("49 days"), T("1 day"), T("97 days")],
                            &[T("2"), T("37500 days"), T("50 days"), T("2 days"), T("98 days")],
                            &[T("3"), T("38250 days"), T("51 days"), T("3 days"), T("99 days")],
                            &[T("5"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, string_agg(d, ',' ORDER BY d) IS NOT NULL AS ordered, length(string_agg(d, ',')), length(string_agg(j, '')) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b, pa.d"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on pa_p1 pa_1"#)],
                            &[T(r#"              ->  Seq Scan on pa_p2 pa_2"#)],
                            &[T(r#"              ->  Seq Scan on pa_p3 pa_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, string_agg(d, ',' ORDER BY d) IS NOT NULL AS ordered, length(string_agg(d, ',')), length(string_agg(j, '')) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("ordered", BOOL), Column("length", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("0"), T("t"), T("4221"), T("750")],
                            &[T("1"), T("t"), T("4221"), T("750")],
                            &[T("2"), T("t"), T("4222"), T("750")],
                            &[T("3"), T("t"), T("4222"), T("750")],
                            &[T("5"), T("f"), Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, length(string_agg(d, ',')), length(string_agg(j, '')), length(string_agg(d, NULL)) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, length(string_agg(d, ',')), length(string_agg(j, '')), length(string_agg(d, NULL)) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("length", INT4), Column("length", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("0"), T("4221"), T("750"), T("3472")],
                            &[T("1"), T("4221"), T("750"), T("3472")],
                            &[T("2"), T("4222"), T("750"), T("3473")],
                            &[T("3"), T("4222"), T("750"), T("3473")],
                            &[T("5"), Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, cardinality(array_agg(a)), cardinality(array_agg(h) FILTER (WHERE h IS NOT NULL)), array_length(array_agg(h) FILTER (WHERE h IS NOT NULL), 1) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, cardinality(array_agg(a)), cardinality(array_agg(h) FILTER (WHERE h IS NOT NULL)), array_length(array_agg(h) FILTER (WHERE h IS NOT NULL), 1) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("cardinality", INT4), Column("cardinality", INT4), Column("array_length", INT4)],
                        rows: &[
                            &[T("0"), T("750"), T("1500"), T("750")],
                            &[T("1"), T("750"), T("1500"), T("750")],
                            &[T("2"), T("750"), T("1500"), T("750")],
                            &[T("3"), T("750"), T("1500"), T("750")],
                            &[T("5"), T("1"), Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, min(a), max(a), min(d), max(d), min(c), max(c), bool_and(k), bool_or(k), every(k), bit_and(a), bit_or(a), bit_xor(a), sum(n), min(n), max(n) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, min(a), max(a), min(d), max(d), min(c), max(c), bool_and(k), bool_or(k), every(k), bit_and(a), bit_or(a), bit_xor(a), sum(n), min(n), max(n) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("min", INT4), Column("max", INT4), Column("min", TEXT), Column("max", TEXT), Column("min", NUMERIC), Column("max", NUMERIC), Column("bool_and", BOOL), Column("bool_or", BOOL), Column("every", BOOL), Column("bit_and", INT4), Column("bit_or", INT4), Column("bit_xor", INT4), Column("sum", MONEY), Column("min", MONEY), Column("max", MONEY)],
                        rows: &[
                            &[T("0"), T("0"), T("28"), T("v0"), T("v996"), T("0.0"), T("4494.0"), T("f"), T("t"), T("f"), T("0"), T("30"), T("0"), T("$1,123,500.00"), T("$0.00"), T("$2,996.00")],
                            &[T("1"), T("1"), T("29"), T("v1"), T("v997"), T("1.5"), T("4495.5"), T("f"), T("t"), T("f"), T("1"), T("31"), T("0"), T("$1,124,250.00"), T("$1.00"), T("$2,997.00")],
                            &[T("2"), T("0"), T("28"), T("v10"), T("v998"), T("3.0"), T("4497.0"), T("f"), T("t"), T("f"), T("0"), T("30"), T("0"), T("$1,125,000.00"), T("$2.00"), T("$2,998.00")],
                            &[T("3"), T("1"), T("29"), T("v1003"), T("v999"), T("4.5"), T("4498.5"), T("f"), T("t"), T("f"), T("1"), T("31"), T("0"), T("$1,125,750.00"), T("$3.00"), T("$2,999.00")],
                            &[T("5"), T("29"), T("29"), Null, Null, Null, Null, Null, Null, Null, T("29"), T("29"), T("29"), Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, sum(c), avg(e), string_agg(d, ','), array_agg(h) FILTER (WHERE h IS NOT NULL) FROM pa GROUP BY b HAVING sum(a) > 50 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  Filter: (sum(pa.a) > 50)"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, sum(c), avg(e), array_length(array_agg(h) FILTER (WHERE h IS NOT NULL), 1) FROM pa GROUP BY b HAVING sum(a) > 50 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("sum", NUMERIC), Column("avg", INTERVAL), Column("array_length", INT4)],
                        rows: &[
                            &[T("0"), T("1685250.0"), T("48 days"), T("750")],
                            &[T("1"), T("1686375.0"), T("49 days"), T("750")],
                            &[T("2"), T("1687500.0"), T("50 days"), T("750")],
                            &[T("3"), T("1688625.0"), T("51 days"), T("750")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashagg TO false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, count(*), avg(c) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Merge Append"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Partial GroupAggregate"#)],
                            &[T(r#"              Group Key: pa.b"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pa.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"        ->  Partial GroupAggregate"#)],
                            &[T(r#"              Group Key: pa_1.b"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pa_1.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"        ->  Partial GroupAggregate"#)],
                            &[T(r#"              Group Key: pa_2.b"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pa_2.b"#)],
                            &[T(r#"                    ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(*), avg(c), avg(g), variance(i) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8), Column("avg", NUMERIC), Column("avg", NUMERIC), Column("variance", NUMERIC)],
                        rows: &[
                            &[T("0"), T("750"), T("2247.0000000000000000"), T("1498000000000000.0000"), T("83443.257676902537")],
                            &[T("1"), T("750"), T("2248.5000000000000000"), T("1499000000000000.0000"), T("83443.257676902537")],
                            &[T("2"), T("750"), T("2250.0000000000000000"), T("1500000000000000.0000"), T("83443.257676902537")],
                            &[T("3"), T("750"), T("2251.5000000000000000"), T("1501000000000000.0000"), T("83443.257676902537")],
                            &[T("5"), T("1"), Null, Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashagg;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, count(*), sum(c) FROM pa GROUP BY a HAVING count(*) > 1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pa.a"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: pa.a"#)],
                            &[T(r#"              Filter: (count(*) > 1)"#)],
                            &[T(r#"              ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: pa_1.a"#)],
                            &[T(r#"              Filter: (count(*) > 1)"#)],
                            &[T(r#"              ->  Seq Scan on pa_p2 pa_1"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: pa_2.a"#)],
                            &[T(r#"              Filter: (count(*) > 1)"#)],
                            &[T(r#"              ->  Seq Scan on pa_p3 pa_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, count(*), sum(c) FROM pa GROUP BY a HAVING count(*) > 1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8), Column("sum", NUMERIC)],
                        rows: &[
                            &[T("0"), T("100"), T("222750.0")],
                            &[T("1"), T("100"), T("222900.0")],
                            &[T("2"), T("100"), T("223050.0")],
                            &[T("3"), T("100"), T("223200.0")],
                            &[T("4"), T("100"), T("223350.0")],
                            &[T("5"), T("100"), T("223500.0")],
                            &[T("6"), T("100"), T("223650.0")],
                            &[T("7"), T("100"), T("223800.0")],
                            &[T("8"), T("100"), T("223950.0")],
                            &[T("9"), T("100"), T("224100.0")],
                            &[T("10"), T("100"), T("224250.0")],
                            &[T("11"), T("100"), T("224400.0")],
                            &[T("12"), T("100"), T("224550.0")],
                            &[T("13"), T("100"), T("224700.0")],
                            &[T("14"), T("100"), T("224850.0")],
                            &[T("15"), T("100"), T("225000.0")],
                            &[T("16"), T("100"), T("225150.0")],
                            &[T("17"), T("100"), T("225300.0")],
                            &[T("18"), T("100"), T("225450.0")],
                            &[T("19"), T("100"), T("225600.0")],
                            &[T("20"), T("100"), T("225750.0")],
                            &[T("21"), T("100"), T("225900.0")],
                            &[T("22"), T("100"), T("226050.0")],
                            &[T("23"), T("100"), T("226200.0")],
                            &[T("24"), T("100"), T("226350.0")],
                            &[T("25"), T("100"), T("226500.0")],
                            &[T("26"), T("100"), T("226650.0")],
                            &[T("27"), T("100"), T("226800.0")],
                            &[T("28"), T("100"), T("226950.0")],
                            &[T("29"), T("101"), T("227100.0")],
                        ],
                        tag: "SELECT 30",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*), sum(c), avg(g), string_agg(d, ',' ORDER BY d) FROM pa;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Aggregate"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.d"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on pa_p1 pa_1"#)],
                            &[T(r#"              ->  Seq Scan on pa_p2 pa_2"#)],
                            &[T(r#"              ->  Seq Scan on pa_p3 pa_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, count(DISTINCT c) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b, pa.c"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on pa_p1 pa_1"#)],
                            &[T(r#"              ->  Seq Scan on pa_p2 pa_2"#)],
                            &[T(r#"              ->  Seq Scan on pa_p3 pa_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(DISTINCT c) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("750")],
                            &[T("1"), T("750")],
                            &[T("2"), T("750")],
                            &[T("3"), T("750")],
                            &[T("5"), T("0")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan TO off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan TO off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, count(*) FROM pa WHERE a < 5 GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: pa.b"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pa.b"#)],
                            &[T(r#"        ->  Partial HashAggregate"#)],
                            &[T(r#"              Group Key: pa.b"#)],
                            &[T(r#"              ->  Seq Scan on pa_p1 pa"#)],
                            &[T(r#"                    Filter: (a < 5)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, count(*) FROM pa WHERE a < 5 GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("150")],
                            &[T("1"), T("100")],
                            &[T("2"), T("150")],
                            &[T("3"), T("100")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_indexscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_bitmapscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_partitionwise_aggregate TO false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, count(*) FROM pa GROUP BY b ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pa.b"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pa.b"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on pa_p1 pa_1"#)],
                            &[T(r#"              ->  Seq Scan on pa_p2 pa_2"#)],
                            &[T(r#"              ->  Seq Scan on pa_p3 pa_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a combine function refuses a call outside an aggregate",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT int4_avg_combine('{1,2}'::INT8[], '{3,4}'::INT8[]);",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "aggregate function called in non-aggregate context", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_partitionwise_aggregates_of_postgres_tests() {
    run_scripts(&[
        ScriptTest {
            name: "partitionwise aggregates of Postgres' partition_aggregate tests",
            set_up_script: &[
                "SET enable_partitionwise_aggregate TO true;",
                "SET enable_partitionwise_join TO true;",
                "SET max_parallel_workers_per_gather TO 0;",
                "SET enable_incremental_sort TO off;",
                "CREATE TABLE pagg_tab (a int, b int, c text, d int) PARTITION BY LIST(c);",
                "CREATE TABLE pagg_tab_p1 PARTITION OF pagg_tab FOR VALUES IN ('0000', '0001', '0002', '0003', '0004');",
                "CREATE TABLE pagg_tab_p2 PARTITION OF pagg_tab FOR VALUES IN ('0005', '0006', '0007', '0008');",
                "CREATE TABLE pagg_tab_p3 PARTITION OF pagg_tab FOR VALUES IN ('0009', '0010', '0011');",
                "INSERT INTO pagg_tab SELECT i % 20, i % 30, to_char(i % 12, 'FM0000'), i % 30 FROM (SELECT d0.v + 10 * d1.v + 100 * d2.v + 1000 * d3.v AS i FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d3(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d2(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d1(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d0(v) WHERE d0.v + 10 * d1.v + 100 * d2.v + 1000 * d3.v BETWEEN 0 AND 2999) gs_i;",
                "ANALYZE pagg_tab;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT c, sum(a), avg(b), count(*), min(a), max(b) FROM pagg_tab GROUP BY c HAVING avg(d) < 15 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab.c, (sum(pagg_tab.a)), (avg(pagg_tab.b))"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab.c"#)],
                            &[T(r#"              Filter: (avg(pagg_tab.d) < '15'::numeric)"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab_1.c"#)],
                            &[T(r#"              Filter: (avg(pagg_tab_1.d) < '15'::numeric)"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab_2.c"#)],
                            &[T(r#"              Filter: (avg(pagg_tab_2.d) < '15'::numeric)"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c, sum(a), avg(b), count(*), min(a), max(b) FROM pagg_tab GROUP BY c HAVING avg(d) < 15 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("c", TEXT), Column("sum", INT8), Column("avg", NUMERIC), Column("count", INT8), Column("min", INT4), Column("max", INT4)],
                        rows: &[
                            &[T("0000"), T("2000"), T("12.0000000000000000"), T("250"), T("0"), T("24")],
                            &[T("0001"), T("2250"), T("13.0000000000000000"), T("250"), T("1"), T("25")],
                            &[T("0002"), T("2500"), T("14.0000000000000000"), T("250"), T("2"), T("26")],
                            &[T("0006"), T("2500"), T("12.0000000000000000"), T("250"), T("2"), T("24")],
                            &[T("0007"), T("2750"), T("13.0000000000000000"), T("250"), T("3"), T("25")],
                            &[T("0008"), T("2000"), T("14.0000000000000000"), T("250"), T("0"), T("26")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, sum(b), avg(b), count(*), min(a), max(b) FROM pagg_tab GROUP BY a HAVING avg(d) < 15 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab.a, (sum(pagg_tab.b)), (avg(pagg_tab.b))"#)],
                            &[T(r#"  ->  Finalize HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab.a"#)],
                            &[T(r#"        Filter: (avg(pagg_tab.d) < '15'::numeric)"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pagg_tab.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pagg_tab_1.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: pagg_tab_2.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, sum(b), avg(b), count(*), min(a), max(b) FROM pagg_tab GROUP BY a HAVING avg(d) < 15 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("sum", INT8), Column("avg", NUMERIC), Column("count", INT8), Column("min", INT4), Column("max", INT4)],
                        rows: &[
                            &[T("0"), T("1500"), T("10.0000000000000000"), T("150"), T("0"), T("20")],
                            &[T("1"), T("1650"), T("11.0000000000000000"), T("150"), T("1"), T("21")],
                            &[T("2"), T("1800"), T("12.0000000000000000"), T("150"), T("2"), T("22")],
                            &[T("3"), T("1950"), T("13.0000000000000000"), T("150"), T("3"), T("23")],
                            &[T("4"), T("2100"), T("14.0000000000000000"), T("150"), T("4"), T("24")],
                            &[T("10"), T("1500"), T("10.0000000000000000"), T("150"), T("10"), T("20")],
                            &[T("11"), T("1650"), T("11.0000000000000000"), T("150"), T("11"), T("21")],
                            &[T("12"), T("1800"), T("12.0000000000000000"), T("150"), T("12"), T("22")],
                            &[T("13"), T("1950"), T("13.0000000000000000"), T("150"), T("13"), T("23")],
                            &[T("14"), T("2100"), T("14.0000000000000000"), T("150"), T("14"), T("24")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, c, count(*) FROM pagg_tab GROUP BY a, c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab.a, pagg_tab.c"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab_1.a, pagg_tab_1.c"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab_2.a, pagg_tab_2.c"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, c, count(*) FROM pagg_tab GROUP BY c, a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab.c, pagg_tab.a"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab_1.c, pagg_tab_1.a"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab_2.c, pagg_tab_2.a"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT c, a, count(*) FROM pagg_tab GROUP BY a, c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Append"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab.a, pagg_tab.c"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab_1.a, pagg_tab_1.c"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab_2.a, pagg_tab_2.c"#)],
                            &[T(r#"        ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT c, sum(a) FROM pagg_tab WHERE 1 = 2 GROUP BY c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"HashAggregate"#)],
                            &[T(r#"  Group Key: c"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c, sum(a) FROM pagg_tab WHERE 1 = 2 GROUP BY c;",
                    expected: Expected::Rows {
                        columns: &[Column("c", TEXT), Column("sum", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT c, sum(a) FROM pagg_tab WHERE c = 'x' GROUP BY c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"GroupAggregate"#)],
                            &[T(r#"  ->  Result"#)],
                            &[T(r#"        One-Time Filter: false"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c, sum(a) FROM pagg_tab WHERE c = 'x' GROUP BY c;",
                    expected: Expected::Rows {
                        columns: &[Column("c", TEXT), Column("sum", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashagg TO false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT c, sum(a), avg(b), count(*) FROM pagg_tab GROUP BY 1 HAVING avg(d) < 15 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab.c, (sum(pagg_tab.a)), (avg(pagg_tab.b))"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab.c"#)],
                            &[T(r#"              Filter: (avg(pagg_tab.d) < '15'::numeric)"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab.c"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab_1.c"#)],
                            &[T(r#"              Filter: (avg(pagg_tab_1.d) < '15'::numeric)"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab_1.c"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab_2.c"#)],
                            &[T(r#"              Filter: (avg(pagg_tab_2.d) < '15'::numeric)"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab_2.c"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c, sum(a), avg(b), count(*) FROM pagg_tab GROUP BY 1 HAVING avg(d) < 15 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("c", TEXT), Column("sum", INT8), Column("avg", NUMERIC), Column("count", INT8)],
                        rows: &[
                            &[T("0000"), T("2000"), T("12.0000000000000000"), T("250")],
                            &[T("0001"), T("2250"), T("13.0000000000000000"), T("250")],
                            &[T("0002"), T("2500"), T("14.0000000000000000"), T("250")],
                            &[T("0006"), T("2500"), T("12.0000000000000000"), T("250")],
                            &[T("0007"), T("2750"), T("13.0000000000000000"), T("250")],
                            &[T("0008"), T("2000"), T("14.0000000000000000"), T("250")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, sum(b), avg(b), count(*) FROM pagg_tab GROUP BY 1 HAVING avg(d) < 15 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab.a, (sum(pagg_tab.b)), (avg(pagg_tab.b))"#)],
                            &[T(r#"  ->  Finalize GroupAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab.a"#)],
                            &[T(r#"        Filter: (avg(pagg_tab.d) < '15'::numeric)"#)],
                            &[T(r#"        ->  Merge Append"#)],
                            &[T(r#"              Sort Key: pagg_tab.a"#)],
                            &[T(r#"              ->  Partial GroupAggregate"#)],
                            &[T(r#"                    Group Key: pagg_tab.a"#)],
                            &[T(r#"                    ->  Sort"#)],
                            &[T(r#"                          Sort Key: pagg_tab.a"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"              ->  Partial GroupAggregate"#)],
                            &[T(r#"                    Group Key: pagg_tab_1.a"#)],
                            &[T(r#"                    ->  Sort"#)],
                            &[T(r#"                          Sort Key: pagg_tab_1.a"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"              ->  Partial GroupAggregate"#)],
                            &[T(r#"                    Group Key: pagg_tab_2.a"#)],
                            &[T(r#"                    ->  Sort"#)],
                            &[T(r#"                          Sort Key: pagg_tab_2.a"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, sum(b), avg(b), count(*) FROM pagg_tab GROUP BY 1 HAVING avg(d) < 15 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("sum", INT8), Column("avg", NUMERIC), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("1500"), T("10.0000000000000000"), T("150")],
                            &[T("1"), T("1650"), T("11.0000000000000000"), T("150")],
                            &[T("2"), T("1800"), T("12.0000000000000000"), T("150")],
                            &[T("3"), T("1950"), T("13.0000000000000000"), T("150")],
                            &[T("4"), T("2100"), T("14.0000000000000000"), T("150")],
                            &[T("10"), T("1500"), T("10.0000000000000000"), T("150")],
                            &[T("11"), T("1650"), T("11.0000000000000000"), T("150")],
                            &[T("12"), T("1800"), T("12.0000000000000000"), T("150")],
                            &[T("13"), T("1950"), T("13.0000000000000000"), T("150")],
                            &[T("14"), T("2100"), T("14.0000000000000000"), T("150")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT c FROM pagg_tab GROUP BY c ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Merge Append"#)],
                            &[T(r#"  Sort Key: pagg_tab.c"#)],
                            &[T(r#"  ->  Group"#)],
                            &[T(r#"        Group Key: pagg_tab.c"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Sort Key: pagg_tab.c"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"  ->  Group"#)],
                            &[T(r#"        Group Key: pagg_tab_1.c"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Sort Key: pagg_tab_1.c"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"  ->  Group"#)],
                            &[T(r#"        Group Key: pagg_tab_2.c"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Sort Key: pagg_tab_2.c"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c FROM pagg_tab GROUP BY c ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("c", TEXT)],
                        rows: &[
                            &[T("0000")],
                            &[T("0001")],
                            &[T("0002")],
                            &[T("0003")],
                            &[T("0004")],
                            &[T("0005")],
                            &[T("0006")],
                            &[T("0007")],
                            &[T("0008")],
                            &[T("0009")],
                            &[T("0010")],
                            &[T("0011")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM pagg_tab WHERE a < 3 GROUP BY a ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Group"#)],
                            &[T(r#"  Group Key: pagg_tab.a"#)],
                            &[T(r#"  ->  Merge Append"#)],
                            &[T(r#"        Sort Key: pagg_tab.a"#)],
                            &[T(r#"        ->  Group"#)],
                            &[T(r#"              Group Key: pagg_tab.a"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"                          Filter: (a < 3)"#)],
                            &[T(r#"        ->  Group"#)],
                            &[T(r#"              Group Key: pagg_tab_1.a"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab_1.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"                          Filter: (a < 3)"#)],
                            &[T(r#"        ->  Group"#)],
                            &[T(r#"              Group Key: pagg_tab_2.a"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab_2.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                            &[T(r#"                          Filter: (a < 3)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM pagg_tab WHERE a < 3 GROUP BY a ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM pagg_tab GROUP BY c ORDER BY c LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Limit"#)],
                            &[T(r#"  ->  Merge Append"#)],
                            &[T(r#"        Sort Key: pagg_tab.c"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab.c"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab.c"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab_1.c"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab_1.c"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab_2.c"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab_2.c"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pagg_tab GROUP BY c ORDER BY c LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("250")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashagg;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT c, sum(a) FROM pagg_tab GROUP BY rollup(c) ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab.c, (sum(pagg_tab.a))"#)],
                            &[T(r#"  ->  MixedAggregate"#)],
                            &[T(r#"        Hash Key: pagg_tab.c"#)],
                            &[T(r#"        Group Key: ()"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p1 pagg_tab_1"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p2 pagg_tab_2"#)],
                            &[T(r#"              ->  Seq Scan on pagg_tab_p3 pagg_tab_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT c, sum(b order by a) FROM pagg_tab GROUP BY c ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab.c, (sum(pagg_tab.b ORDER BY pagg_tab.a))"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab.c"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab.c, pagg_tab.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p1 pagg_tab"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab_1.c"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab_1.c, pagg_tab_1.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p2 pagg_tab_1"#)],
                            &[T(r#"        ->  GroupAggregate"#)],
                            &[T(r#"              Group Key: pagg_tab_2.c"#)],
                            &[T(r#"              ->  Sort"#)],
                            &[T(r#"                    Sort Key: pagg_tab_2.c, pagg_tab_2.a"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p3 pagg_tab_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, sum(b order by a) FROM pagg_tab GROUP BY a ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab.a, (sum(pagg_tab.b ORDER BY pagg_tab.a))"#)],
                            &[T(r#"  ->  GroupAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab.a"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Sort Key: pagg_tab.a"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p1 pagg_tab_1"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p2 pagg_tab_2"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab_p3 pagg_tab_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pagg_tab1(x int, y int) PARTITION BY RANGE(x);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pagg_tab1_p1 PARTITION OF pagg_tab1 FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pagg_tab1_p2 PARTITION OF pagg_tab1 FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pagg_tab1_p3 PARTITION OF pagg_tab1 FOR VALUES FROM (20) TO (30);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pagg_tab2(x int, y int) PARTITION BY RANGE(y);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pagg_tab2_p1 PARTITION OF pagg_tab2 FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pagg_tab2_p2 PARTITION OF pagg_tab2 FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pagg_tab2_p3 PARTITION OF pagg_tab2 FOR VALUES FROM (20) TO (30);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pagg_tab1 SELECT i % 30, i % 20 FROM (SELECT d0.v + 10 * d1.v + 100 * d2.v AS i FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d2(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d1(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d0(v) WHERE d0.v + 10 * d1.v + 100 * d2.v BETWEEN 0 AND 299 AND (d0.v + 10 * d1.v + 100 * d2.v - 0) % 2 = 0) gs_i;",
                    expected: Expected::Tag("INSERT 0 150"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pagg_tab2 SELECT i % 20, i % 30 FROM (SELECT d0.v + 10 * d1.v + 100 * d2.v AS i FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d2(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d1(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) d0(v) WHERE d0.v + 10 * d1.v + 100 * d2.v BETWEEN 0 AND 299 AND (d0.v + 10 * d1.v + 100 * d2.v - 0) % 3 = 0) gs_i;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE pagg_tab1;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE pagg_tab2;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.x, sum(t1.y), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.x ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.x, (sum(t1.y)), (count(*))"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: t1.x"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t1.x = t2.y)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p1 t1"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p1 t2"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: t1_1.x"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t1_1.x = t2_1.y)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p2 t1_1"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p2 t2_1"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: t1_2.x"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t2_2.y = t1_2.x)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab2_p3 t2_2"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab1_p3 t1_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.x, sum(t1.y), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.x ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("500"), T("100")],
                            &[T("6"), T("1100"), T("100")],
                            &[T("12"), T("700"), T("100")],
                            &[T("18"), T("1300"), T("100")],
                            &[T("24"), T("900"), T("100")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.x, sum(t1.y), count(t1) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.x ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.x, (sum(t1.y)), (count(((t1.*)::pagg_tab1)))"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: t1.x"#)],
                            &[T(r#"        ->  Hash Join"#)],
                            &[T(r#"              Hash Cond: (t1.x = t2.y)"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p1 t1_1"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p2 t1_2"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p3 t1_3"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Append"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p1 t2_1"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p2 t2_2"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p3 t2_3"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.x, sum(t1.y), count(t1) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.x ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("500"), T("100")],
                            &[T("6"), T("1100"), T("100")],
                            &[T("12"), T("700"), T("100")],
                            &[T("18"), T("1300"), T("100")],
                            &[T("24"), T("900"), T("100")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t2.y, sum(t1.y), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t2.y ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t2.y, (sum(t1.y)), (count(*))"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: t2.y"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t1.x = t2.y)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p1 t1"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p1 t2"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: t2_1.y"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t1_1.x = t2_1.y)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p2 t1_1"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p2 t2_1"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: t2_2.y"#)],
                            &[T(r#"              ->  Hash Join"#)],
                            &[T(r#"                    Hash Cond: (t2_2.y = t1_2.x)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab2_p3 t2_2"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab1_p3 t1_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashagg TO false;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t1.y, sum(t1.x), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.y HAVING avg(t1.x) > 10 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: t1.y, (sum(t1.x)), (count(*))"#)],
                            &[T(r#"  ->  Finalize GroupAggregate"#)],
                            &[T(r#"        Group Key: t1.y"#)],
                            &[T(r#"        Filter: (avg(t1.x) > '10'::numeric)"#)],
                            &[T(r#"        ->  Merge Append"#)],
                            &[T(r#"              Sort Key: t1.y"#)],
                            &[T(r#"              ->  Partial GroupAggregate"#)],
                            &[T(r#"                    Group Key: t1.y"#)],
                            &[T(r#"                    ->  Sort"#)],
                            &[T(r#"                          Sort Key: t1.y"#)],
                            &[T(r#"                          ->  Hash Join"#)],
                            &[T(r#"                                Hash Cond: (t1.x = t2.y)"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab1_p1 t1"#)],
                            &[T(r#"                                ->  Hash"#)],
                            &[T(r#"                                      ->  Seq Scan on pagg_tab2_p1 t2"#)],
                            &[T(r#"              ->  Partial GroupAggregate"#)],
                            &[T(r#"                    Group Key: t1_1.y"#)],
                            &[T(r#"                    ->  Sort"#)],
                            &[T(r#"                          Sort Key: t1_1.y"#)],
                            &[T(r#"                          ->  Hash Join"#)],
                            &[T(r#"                                Hash Cond: (t1_1.x = t2_1.y)"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab1_p2 t1_1"#)],
                            &[T(r#"                                ->  Hash"#)],
                            &[T(r#"                                      ->  Seq Scan on pagg_tab2_p2 t2_1"#)],
                            &[T(r#"              ->  Partial GroupAggregate"#)],
                            &[T(r#"                    Group Key: t1_2.y"#)],
                            &[T(r#"                    ->  Sort"#)],
                            &[T(r#"                          Sort Key: t1_2.y"#)],
                            &[T(r#"                          ->  Hash Join"#)],
                            &[T(r#"                                Hash Cond: (t2_2.y = t1_2.x)"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab2_p3 t2_2"#)],
                            &[T(r#"                                ->  Hash"#)],
                            &[T(r#"                                      ->  Seq Scan on pagg_tab1_p3 t1_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t1.y, sum(t1.x), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.y HAVING avg(t1.x) > 10 ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4), Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("2"), T("600"), T("50")],
                            &[T("4"), T("1200"), T("50")],
                            &[T("8"), T("900"), T("50")],
                            &[T("12"), T("600"), T("50")],
                            &[T("14"), T("1200"), T("50")],
                            &[T("18"), T("900"), T("50")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashagg;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b.y, sum(a.y) FROM pagg_tab1 a LEFT JOIN pagg_tab2 b ON a.x = b.y GROUP BY b.y ORDER BY 1 NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: b.y"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: b.y"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: b.y"#)],
                            &[T(r#"                    ->  Hash Left Join"#)],
                            &[T(r#"                          Hash Cond: (a.x = b.y)"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab1_p1 a"#)],
                            &[T(r#"                          ->  Hash"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab2_p1 b"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: b_1.y"#)],
                            &[T(r#"                    ->  Hash Left Join"#)],
                            &[T(r#"                          Hash Cond: (a_1.x = b_1.y)"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab1_p2 a_1"#)],
                            &[T(r#"                          ->  Hash"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab2_p2 b_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: b_2.y"#)],
                            &[T(r#"                    ->  Hash Right Join"#)],
                            &[T(r#"                          Hash Cond: (b_2.y = a_2.x)"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p3 b_2"#)],
                            &[T(r#"                          ->  Hash"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab1_p3 a_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b.y, sum(a.y) FROM pagg_tab1 a LEFT JOIN pagg_tab2 b ON a.x = b.y GROUP BY b.y ORDER BY 1 NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), T("500")],
                            &[T("6"), T("1100")],
                            &[T("12"), T("700")],
                            &[T("18"), T("1300")],
                            &[T("24"), T("900")],
                            &[Null, T("900")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b.y, sum(a.y) FROM pagg_tab1 a RIGHT JOIN pagg_tab2 b ON a.x = b.y GROUP BY b.y ORDER BY 1 NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: b.y"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: b.y"#)],
                            &[T(r#"              ->  Hash Right Join"#)],
                            &[T(r#"                    Hash Cond: (a.x = b.y)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p1 a"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p1 b"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: b_1.y"#)],
                            &[T(r#"              ->  Hash Right Join"#)],
                            &[T(r#"                    Hash Cond: (a_1.x = b_1.y)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p2 a_1"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p2 b_1"#)],
                            &[T(r#"        ->  HashAggregate"#)],
                            &[T(r#"              Group Key: b_2.y"#)],
                            &[T(r#"              ->  Hash Left Join"#)],
                            &[T(r#"                    Hash Cond: (b_2.y = a_2.x)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab2_p3 b_2"#)],
                            &[T(r#"                    ->  Hash"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab1_p3 a_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b.y, sum(a.y) FROM pagg_tab1 a RIGHT JOIN pagg_tab2 b ON a.x = b.y GROUP BY b.y ORDER BY 1 NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), T("500")],
                            &[T("3"), Null],
                            &[T("6"), T("1100")],
                            &[T("9"), Null],
                            &[T("12"), T("700")],
                            &[T("15"), Null],
                            &[T("18"), T("1300")],
                            &[T("21"), Null],
                            &[T("24"), T("900")],
                            &[T("27"), Null],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a.x, sum(b.x) FROM pagg_tab1 a FULL OUTER JOIN pagg_tab2 b ON a.x = b.y GROUP BY a.x ORDER BY 1 NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize GroupAggregate"#)],
                            &[T(r#"  Group Key: a.x"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: a.x"#)],
                            &[T(r#"        ->  Append"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: a.x"#)],
                            &[T(r#"                    ->  Hash Full Join"#)],
                            &[T(r#"                          Hash Cond: (a.x = b.y)"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab1_p1 a"#)],
                            &[T(r#"                          ->  Hash"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab2_p1 b"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: a_1.x"#)],
                            &[T(r#"                    ->  Hash Full Join"#)],
                            &[T(r#"                          Hash Cond: (a_1.x = b_1.y)"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab1_p2 a_1"#)],
                            &[T(r#"                          ->  Hash"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab2_p2 b_1"#)],
                            &[T(r#"              ->  Partial HashAggregate"#)],
                            &[T(r#"                    Group Key: a_2.x"#)],
                            &[T(r#"                    ->  Hash Full Join"#)],
                            &[T(r#"                          Hash Cond: (b_2.y = a_2.x)"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p3 b_2"#)],
                            &[T(r#"                          ->  Hash"#)],
                            &[T(r#"                                ->  Seq Scan on pagg_tab1_p3 a_2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.x, sum(b.x) FROM pagg_tab1 a FULL OUTER JOIN pagg_tab2 b ON a.x = b.y GROUP BY a.x ORDER BY 1 NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("sum", INT8)],
                        rows: &[
                            &[T("0"), T("500")],
                            &[T("2"), Null],
                            &[T("4"), Null],
                            &[T("6"), T("1100")],
                            &[T("8"), Null],
                            &[T("10"), Null],
                            &[T("12"), T("700")],
                            &[T("14"), Null],
                            &[T("16"), Null],
                            &[T("18"), T("1300")],
                            &[T("20"), Null],
                            &[T("22"), Null],
                            &[T("24"), T("900")],
                            &[T("26"), Null],
                            &[T("28"), Null],
                            &[Null, T("500")],
                        ],
                        tag: "SELECT 16",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a.x, b.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x < 20) a LEFT JOIN (SELECT * FROM pagg_tab2 WHERE y > 10) b ON a.x = b.y WHERE a.x > 5 or b.y < 20  GROUP BY a.x, b.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab1.x, pagg_tab2.y"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab1.x, pagg_tab2.y"#)],
                            &[T(r#"        ->  Hash Left Join"#)],
                            &[T(r#"              Hash Cond: (pagg_tab1.x = pagg_tab2.y)"#)],
                            &[T(r#"              Filter: ((pagg_tab1.x > 5) OR (pagg_tab2.y < 20))"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p1 pagg_tab1_1"#)],
                            &[T(r#"                          Filter: (x < 20)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p2 pagg_tab1_2"#)],
                            &[T(r#"                          Filter: (x < 20)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Append"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p2 pagg_tab2_1"#)],
                            &[T(r#"                                Filter: (y > 10)"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p3 pagg_tab2_2"#)],
                            &[T(r#"                                Filter: (y > 10)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.x, b.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x < 20) a LEFT JOIN (SELECT * FROM pagg_tab2 WHERE y > 10) b ON a.x = b.y WHERE a.x > 5 or b.y < 20  GROUP BY a.x, b.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("6"), Null, T("10")],
                            &[T("8"), Null, T("10")],
                            &[T("10"), Null, T("10")],
                            &[T("12"), T("12"), T("100")],
                            &[T("14"), Null, T("10")],
                            &[T("16"), Null, T("10")],
                            &[T("18"), T("18"), T("100")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a.x, b.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x < 20) a FULL JOIN (SELECT * FROM pagg_tab2 WHERE y > 10) b ON a.x = b.y WHERE a.x > 5 or b.y < 20  GROUP BY a.x, b.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Sort"#)],
                            &[T(r#"  Sort Key: pagg_tab1.x, pagg_tab2.y"#)],
                            &[T(r#"  ->  HashAggregate"#)],
                            &[T(r#"        Group Key: pagg_tab1.x, pagg_tab2.y"#)],
                            &[T(r#"        ->  Hash Full Join"#)],
                            &[T(r#"              Hash Cond: (pagg_tab1.x = pagg_tab2.y)"#)],
                            &[T(r#"              Filter: ((pagg_tab1.x > 5) OR (pagg_tab2.y < 20))"#)],
                            &[T(r#"              ->  Append"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p1 pagg_tab1_1"#)],
                            &[T(r#"                          Filter: (x < 20)"#)],
                            &[T(r#"                    ->  Seq Scan on pagg_tab1_p2 pagg_tab1_2"#)],
                            &[T(r#"                          Filter: (x < 20)"#)],
                            &[T(r#"              ->  Hash"#)],
                            &[T(r#"                    ->  Append"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p2 pagg_tab2_1"#)],
                            &[T(r#"                                Filter: (y > 10)"#)],
                            &[T(r#"                          ->  Seq Scan on pagg_tab2_p3 pagg_tab2_2"#)],
                            &[T(r#"                                Filter: (y > 10)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.x, b.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x < 20) a FULL JOIN (SELECT * FROM pagg_tab2 WHERE y > 10) b ON a.x = b.y WHERE a.x > 5 or b.y < 20 GROUP BY a.x, b.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("6"), Null, T("10")],
                            &[T("8"), Null, T("10")],
                            &[T("10"), Null, T("10")],
                            &[T("12"), T("12"), T("100")],
                            &[T("14"), Null, T("10")],
                            &[T("16"), Null, T("10")],
                            &[T("18"), T("18"), T("100")],
                            &[Null, T("15"), T("10")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a.x, a.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x = 1 AND x = 2) a LEFT JOIN pagg_tab2 b ON a.x = b.y GROUP BY a.x, a.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"GroupAggregate"#)],
                            &[T(r#"  Group Key: pagg_tab1.y"#)],
                            &[T(r#"  ->  Sort"#)],
                            &[T(r#"        Sort Key: pagg_tab1.y"#)],
                            &[T(r#"        ->  Result"#)],
                            &[T(r#"              One-Time Filter: false"#)],
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
fn test_partition_bound_values_that_need_evaluating() {
    run_scripts(&[
        ScriptTest {
            name: "partition bound values that need evaluating are evaluated with the session's settings",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET TimeZone = 'UTC';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tz (t TIMESTAMPTZ) PARTITION BY RANGE (t);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tz1 PARTITION OF tz FOR VALUES FROM ('2026-01-01 00:00'::TEXT::TIMESTAMPTZ) TO ('2026-01-02 00:00'::TEXT::TIMESTAMPTZ);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TimeZone = 'America/New_York';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tz2 PARTITION OF tz FOR VALUES FROM ('2026-01-02 00:00'::TEXT::TIMESTAMPTZ) TO ('2026-01-03 00:00'::TEXT::TIMESTAMPTZ);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TimeZone = 'UTC';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tz VALUES ('2026-01-01 12:00+00'), ('2026-01-02 06:00+00'), ('2026-01-03 04:59+00');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tz VALUES ('2026-01-02 02:00+00');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "tz" found for row"#, detail: "Partition key of the failing row contains (t) = (2026-01-02 02:00:00+00).", schema: "public", table: "tz", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, t FROM tz ORDER BY t;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("t", TIMESTAMPTZ)],
                        rows: &[
                            &[T("tz1"), T("2026-01-01 12:00:00+00")],
                            &[T("tz2"), T("2026-01-02 06:00:00+00")],
                            &[T("tz2"), T("2026-01-03 04:59:00+00")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tz3 PARTITION OF tz FOR VALUES FROM ('2026-01-02 03:00'::TEXT::TIMESTAMPTZ) TO ('2026-01-02 06:00'::TEXT::TIMESTAMPTZ);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"partition "tz3" would overlap partition "tz2""#, position: 94, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE alpha (a DOUBLE PRECISION, b INT, c TEXT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE alpha_neg PARTITION OF alpha FOR VALUES FROM ('-Infinity') TO (0);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE alpha_pos PARTITION OF alpha FOR VALUES FROM (0) TO (10.0);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO alpha VALUES ('-Infinity', 1, 'x'), (-1.5, 2, 'y'), (0, 3, 'z'), (9.5, 4, 'w');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM alpha ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", FLOAT8)],
                        rows: &[
                            &[T("alpha_neg"), T("-Infinity")],
                            &[T("alpha_neg"), T("-1.5")],
                            &[T("alpha_pos"), T("0")],
                            &[T("alpha_pos"), T("9.5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO alpha VALUES (10, 5, 'v');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"no partition of relation "alpha" found for row"#, detail: "Partition key of the failing row contains (a) = (10).", schema: "public", table: "alpha", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE alpha_x PARTITION OF alpha FOR VALUES FROM (5) TO (20);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"partition "alpha_x" would overlap partition "alpha_pos""#, position: 58, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE alpha_y PARTITION OF alpha FOR VALUES FROM (1 / 0) TO (20);",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE li (a INT) PARTITION BY LIST (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE li1 PARTITION OF li FOR VALUES IN (1 + 1, abs(-3));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO li VALUES (2), (3);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a FROM li ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4)],
                        rows: &[
                            &[T("li1"), T("2")],
                            &[T("li1"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE li2 PARTITION OF li FOR VALUES IN ('2'::TEXT::INT);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"partition "li2" would overlap partition "li1""#, position: 49, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_hash_partitioning_by_an_enum() {
    run_scripts(&[
        ScriptTest {
            name: "hash partitioning by an enum routes, prunes, checks, and indexes each label alike",
            set_up_script: &[
                "CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');",
                "CREATE TABLE moods (m mood, n int) PARTITION BY HASH (m);",
                "CREATE TABLE moods_0 PARTITION OF moods FOR VALUES WITH (MODULUS 2, REMAINDER 0);",
                "CREATE TABLE moods_1 PARTITION OF moods FOR VALUES WITH (MODULUS 2, REMAINDER 1);",
                "INSERT INTO moods SELECT m, n FROM unnest(enum_range(NULL::mood)) m, generate_series(1, 3) n;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT m, count(DISTINCT tableoid) AS partitions, count(*) FROM moods GROUP BY m ORDER BY m;",
                    expected: Expected::Rows {
                        columns: &[Column("m", USER_DEFINED), Column("partitions", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("sad"), T("1"), T("3")],
                            &[T("ok"), T("1"), T("3")],
                            &[T("happy"), T("1"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM moods WHERE m = 'ok';",
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
                    query: "SELECT bool_and(satisfies_hash_partition('moods'::regclass, 2, CASE WHEN tableoid = 'moods_0'::regclass THEN 0 ELSE 1 END, m)) FROM moods;",
                    expected: Expected::Rows {
                        columns: &[Column("bool_and", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX moods_m_hash ON moods USING hash (m);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM moods WHERE m = 'happy';",
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

#[test]
fn test_default_partition_checks_rows_committed_after_a_repeatable_read_snapshot() {
    run_scripts(&[
        ScriptTest {
            name: "a REPEATABLE READ transaction's new partition checks the default partition's rows another client committed after its snapshot, as Postgres' check_default_partition_contents reads with GetLatestSnapshot",
            set_up_script: &[
                "CREATE TABLE lpart (a INT) PARTITION BY LIST (a)",
                "CREATE TABLE lpart_def PARTITION OF lpart DEFAULT",
                "CREATE TABLE lpart_other (a INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM lpart_other",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO lpart VALUES (7)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ CREATE TABLE lpart_7 PARTITION OF lpart FOR VALUES IN (7)",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "updated partition constraint for default partition \"lpart_def\" would be violated by some row", schema: "public", table: "lpart_def", ..E }),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_partition_pruning_with_no_listed_bounds() {
    run_scripts(&[ScriptTest {
        name: "partition pruning of a list-partitioned table whose only partitions take NULL and the rest",
            set_up_script: &[
                "CREATE TABLE exists_part (c1 int, c2 int, c3 int) PARTITION BY LIST (c1);",
                "CREATE TABLE exists_part_null PARTITION OF exists_part FOR VALUES IN (NULL);",
                "CREATE TABLE exists_part_def PARTITION OF exists_part DEFAULT;",
                "INSERT INTO exists_part SELECT x, x / 2, x + 1 FROM generate_series(0, 10) x;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE exists_part;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM exists_part t1 WHERE (EXISTS (SELECT 1 FROM exists_part t2 WHERE t1.c1 = t2.c2) OR c3 < 0);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Seq Scan on exists_part_null t1_1")],
                            &[T("        Filter: (EXISTS(SubPlan 1) OR (c3 < 0))")],
                            &[T("        SubPlan 1")],
                            &[T("          ->  Append")],
                            &[T("                ->  Seq Scan on exists_part_null t2_1")],
                            &[T("                      Filter: (t1_1.c1 = c2)")],
                            &[T("                ->  Seq Scan on exists_part_def t2_2")],
                            &[T("                      Filter: (t1_1.c1 = c2)")],
                            &[T("  ->  Seq Scan on exists_part_def t1_2")],
                            &[T("        Filter: ((ANY (c1 = (hashed SubPlan 2).col1)) OR (c3 < 0))")],
                            &[T("        SubPlan 2")],
                            &[T("          ->  Append")],
                            &[T("                ->  Seq Scan on exists_part_null t2_4")],
                            &[T("                ->  Seq Scan on exists_part_def t2_5")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM exists_part t1 WHERE (EXISTS (SELECT 1 FROM exists_part t2 WHERE t1.c1 = t2.c2) OR c3 < 0) ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("c1", INT4), Column("c2", INT4), Column("c3", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("1")],
                            &[T("1"), T("0"), T("2")],
                            &[T("2"), T("1"), T("3")],
                            &[T("3"), T("1"), T("4")],
                            &[T("4"), T("2"), T("5")],
                            &[T("5"), T("2"), T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM exists_part WHERE c1 = 3;",
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
                    query: "SELECT count(*) FROM exists_part WHERE c1 IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
        ..S
    }]);
}

#[test]
fn test_explain_disabled_appends() {
    run_scripts(&[
        ScriptTest {
            name: "an Append or Merge Append shows Disabled only when it has more disabled nodes than its children",
            set_up_script: &[
                "CREATE TABLE matest0 (id serial PRIMARY KEY, name text)",
                "CREATE TABLE matest1 (id integer PRIMARY KEY) INHERITS (matest0)",
                "CREATE TABLE matest2 (id integer PRIMARY KEY) INHERITS (matest0)",
                "CREATE TABLE matest3 (id integer PRIMARY KEY) INHERITS (matest0)",
                "CREATE INDEX matest0i ON matest0 ((1 - id))",
                "CREATE INDEX matest1i ON matest1 ((1 - id))",
                "CREATE INDEX matest3i ON matest3 ((1 - id))",
                "INSERT INTO matest1 (name) VALUES ('Test 1'), ('Test 2')",
                "INSERT INTO matest2 (name) VALUES ('Test 3'), ('Test 4')",
                "INSERT INTO matest3 (name) VALUES ('Test 5'), ('Test 6')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_parallel_append = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM matest0 ORDER BY 1 - id;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Merge Append")],
                            &[T("  Sort Key: ((1 - matest0.id))")],
                            &[T("  ->  Index Scan using matest0i on matest0 matest0_1")],
                            &[T("  ->  Index Scan using matest1i on matest1 matest0_2")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: ((1 - matest0_3.id))")],
                            &[T("        ->  Seq Scan on matest2 matest0_3")],
                            &[T("              Disabled: true")],
                            &[T("  ->  Index Scan using matest3i on matest3 matest0_4")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM matest0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Seq Scan on matest0 matest0_1")],
                            &[T("        Disabled: true")],
                            &[T("  ->  Seq Scan on matest1 matest0_2")],
                            &[T("        Disabled: true")],
                            &[T("  ->  Seq Scan on matest2 matest0_3")],
                            &[T("        Disabled: true")],
                            &[T("  ->  Seq Scan on matest3 matest0_4")],
                            &[T("        Disabled: true")],
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
fn test_partition_moving_update_waits_for_a_concurrent_update_under_a_before_delete_trigger() {
    run_scripts(&[
        ScriptTest {
            name: "an UPDATE moving a row another transaction updated rechecks it once and moves it after that transaction commits",
            set_up_script: &[
                "CREATE TABLE footrg (a INT, b TEXT) PARTITION BY LIST (a);",
                "CREATE TABLE triglog AS SELECT * FROM footrg;",
                "CREATE TABLE footrg1 PARTITION OF footrg FOR VALUES IN (1);",
                "CREATE TABLE footrg2 PARTITION OF footrg FOR VALUES IN (2);",
                "INSERT INTO footrg VALUES (1, 'ABC');",
                "CREATE FUNCTION func_footrg() RETURNS TRIGGER AS $$ BEGIN OLD.b = OLD.b || ' trigger'; INSERT INTO triglog SELECT OLD.*; RETURN OLD; END $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER footrg_ondel BEFORE DELETE ON footrg1 FOR EACH ROW EXECUTE PROCEDURE func_footrg();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE footrg SET b = b || ' update2' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE footrg SET a = a + 1, b = b || ' update1' WHERE b LIKE '%ABC%';",
                    expected: Expected::Tag("UPDATE 1"),
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM footrg ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("footrg2"), T("2"), T("ABC update2 update1")]],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM triglog ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("1"), T("ABC update2 trigger")]],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_routing_after_waiting_for_a_concurrent_attach() {
    run_scripts(&[
        ScriptTest {
            name: "rows routed to the default partition after waiting for an ATTACH meet its new partition constraint",
            set_up_script: &[
                "CREATE TABLE tpart (i INT, j TEXT) PARTITION BY RANGE (i);",
                "CREATE TABLE tpart_1 (LIKE tpart);",
                "CREATE TABLE tpart_2 (LIKE tpart);",
                "CREATE TABLE tpart_default (a INT, j TEXT, i INT) PARTITION BY LIST (j);",
                "CREATE TABLE tpart_default_default (a INT, i INT, b INT, j TEXT);",
                "ALTER TABLE tpart_default_default DROP b;",
                "ALTER TABLE tpart_default ATTACH PARTITION tpart_default_default DEFAULT;",
                "ALTER TABLE tpart_default DROP a;",
                "ALTER TABLE tpart ATTACH PARTITION tpart_default DEFAULT;",
                "ALTER TABLE tpart ATTACH PARTITION tpart_1 FOR VALUES FROM (0) TO (100);",
                "INSERT INTO tpart_2 VALUES (110, 'xxx'), (120, 'yyy'), (150, 'zzz');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tpart ATTACH PARTITION tpart_2 FOR VALUES FROM (100) TO (200);",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tpart VALUES (110, 'xxx'), (120, 'yyy'), (150, 'zzz');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"tpart_default\" violates partition constraint", ..E }),
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM tpart ORDER BY i;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("i", INT4), Column("j", TEXT)],
                        rows: &[
                            &[T("tpart_2"), T("110"), T("xxx")],
                            &[T("tpart_2"), T("120"), T("yyy")],
                            &[T("tpart_2"), T("150"), T("zzz")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_satisfies_hash_partition_variadic_type_errors() {
    run_scripts(&[
        ScriptTest {
            name: "satisfies_hash_partition quotes the types a variadic array mismatches",
            set_up_script: &[
                "CREATE TABLE mchash (a INT, b TEXT) PARTITION BY HASH (a, b);",
                "CREATE TABLE mcinthash (a INT, b INT) PARTITION BY HASH (a, b);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT satisfies_hash_partition('mchash'::regclass, 2, 1, VARIADIC ARRAY[1,2]::INT[]);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"column 2 of the partition key has type "text", but supplied value is of type "integer""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT satisfies_hash_partition('mcinthash'::regclass, 4, 0, VARIADIC ARRAY[now(), now()]);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"column 1 of the partition key has type "integer", but supplied value is of type "timestamp with time zone""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT satisfies_hash_partition('mchash'::regclass, 2, 1, NULL::INT, NULL::INT);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "column 2 of the partition key has type text, but supplied value is of type integer", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_drop_partitioned_table_keyed_by_its_row_type() {
    run_scripts(&[
        ScriptTest {
            name: "dropping a table partitioned by a cast to its own row type takes out its partition key",
            set_up_script: &[
                "CREATE TABLE p (a INT, b INT) PARTITION BY LIST ((ROW(a, b)::p));",
                "CREATE TABLE p1 PARTITION OF p FOR VALUES IN ('(1,2)');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE p;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_partitioned_table;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            server_config: "debug:\n  check_every: true\n",
            ..S
        },
    ]);
}

#[test]
fn test_partitioned_partitions_keep_relhasindex_beside_cloned_triggers() {
    run_scripts(&[
        ScriptTest {
            name: "a partition that is itself partitioned keeps relhasindex for the index it clones when it also clones the parent's row trigger, which pg_dump needs to dump its key",
            set_up_script: &[
                "CREATE TABLE rh4 (a INT PRIMARY KEY) PARTITION BY LIST (a);",
                "CREATE FUNCTION rh4f() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NULL; END $$;",
                "CREATE TRIGGER rh4t AFTER INSERT ON rh4 FOR EACH ROW EXECUTE FUNCTION rh4f();",
                "CREATE TABLE rh4_1 PARTITION OF rh4 FOR VALUES IN (1);",
                "CREATE TABLE rh4_2 PARTITION OF rh4 FOR VALUES IN (2) PARTITION BY LIST (a);",
                "CREATE TABLE rh4_2_1 PARTITION OF rh4_2 FOR VALUES IN (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relname, relhasindex, relhastriggers FROM pg_class WHERE relname LIKE 'rh4%' AND relkind IN ('r', 'p') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relhasindex", BOOL), Column("relhastriggers", BOOL)],
                        rows: &[
                            &[T("rh4"), T("t"), T("t")],
                            &[T("rh4_1"), T("t"), T("t")],
                            &[T("rh4_2"), T("t"), T("t")],
                            &[T("rh4_2_1"), T("t"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
