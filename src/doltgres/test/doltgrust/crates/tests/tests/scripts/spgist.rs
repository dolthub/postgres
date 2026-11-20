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
use harness::script::{A, Column, Diagnostic, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_spgist_points() {
    run_scripts(&[
        ScriptTest {
            name: "quad_point_ops and kd_point_ops find points by every strategy, NULLs, and distance, kept as GiST's point_ops trees",
            set_up_script: &[
                "CREATE TABLE spp (id INT PRIMARY KEY, p POINT);",
                "INSERT INTO spp SELECT i, point(i % 37, i % 41) FROM generate_series(1, 2000) i;",
                "INSERT INTO spp VALUES (2001, NULL), (2002, NULL);",
                "CREATE INDEX spp_p ON spp USING spgist (p);",
                "CREATE TABLE spk (id INT PRIMARY KEY, p POINT);",
                "INSERT INTO spk SELECT i, point(i % 37, i % 41) FROM generate_series(1, 2000) i;",
                "CREATE INDEX spk_p ON spk USING spgist (p kd_point_ops);",
                "VACUUM ANALYZE spp, spk;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spp WHERE p <@ box '(10,10),(20,20)'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Index Only Scan using spp_p on spp")],
                            &[T("        Index Cond: (p <@ '(20,20),(10,10)'::box)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p <@ box '(10,10),(20,20)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("172")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p << '(5,5)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("272")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p >> '(30,5)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("324")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p <^ '(5,5)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("244")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p >^ '(5,35)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("240")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p ~= '(3,4)'",
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
                    query: "SELECT count(*) FROM spp WHERE p <<| '(5,5)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("244")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p |>> '(5,35)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("240")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p IS NULL",
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
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spp WHERE p IS NULL",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Index Only Scan using spp_p on spp")],
                            &[T("        Index Cond: (p IS NULL)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT p FROM spp ORDER BY p <-> '(7,7)' LIMIT 3",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Index Only Scan using spp_p on spp")],
                            &[T("        Order By: (p <-> '(7,7)'::point)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p, p <-> '(7.2,7.1)' FROM spp ORDER BY p <-> '(7.2,7.1)' LIMIT 3",
                    expected: Expected::Rows {
                        columns: &[Column("p", POINT), Column("?column?", FLOAT8)],
                        rows: &[
                            &[T("(7,7)"), T("0.22360679774997896")],
                            &[T("(7,7)"), T("0.22360679774997896")],
                            &[T("(8,7)"), T("0.8062257748298547")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spk WHERE p <@ box '(10,10),(20,20)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("172")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spk WHERE p >^ '(5,35)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("240")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spk WHERE p ~= '(3,4)'",
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
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spk WHERE p >^ '(5,35)'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Index Only Scan using spk_p on spk")],
                            &[T("        Index Cond: (p >^ '(5,35)'::point)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT p FROM spk ORDER BY p <-> '(7.2,7.1)' LIMIT 3",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Index Only Scan using spk_p on spk")],
                            &[T("        Order By: (p <-> '(7.2,7.1)'::point)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p, p <-> '(7.2,7.1)' FROM spk ORDER BY p <-> '(7.2,7.1)' LIMIT 3",
                    expected: Expected::Rows {
                        columns: &[Column("p", POINT), Column("?column?", FLOAT8)],
                        rows: &[
                            &[T("(7,7)"), T("0.22360679774997896")],
                            &[T("(7,7)"), T("0.22360679774997896")],
                            &[T("(8,7)"), T("0.8062257748298547")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexonlyscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spp WHERE p <@ box '(10,10),(20,20)'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Bitmap Heap Scan on spp")],
                            &[T("        Recheck Cond: (p <@ '(20,20),(10,10)'::box)")],
                            &[T("        ->  Bitmap Index Scan on spp_p")],
                            &[T("              Index Cond: (p <@ '(20,20),(10,10)'::box)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spp WHERE p <@ box '(10,10),(20,20)'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Bitmap Heap Scan on spp")],
                            &[T("        Recheck Cond: (p <@ '(20,20),(10,10)'::box)")],
                            &[T("        ->  Bitmap Index Scan on spp_p")],
                            &[T("              Index Cond: (p <@ '(20,20),(10,10)'::box)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spp WHERE p <@ box '(10,10),(20,20)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("172")],
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
fn test_spgist_boxes_and_polygons() {
    run_scripts(&[
        ScriptTest {
            name: "box_ops and poly_ops find boxes and polygons by every strategy and order them by distance to a point",
            set_up_script: &[
                "CREATE TABLE spb (id INT PRIMARY KEY, b BOX, g POLYGON);",
                "INSERT INTO spb SELECT i, box(point(i % 50, i % 30), point(i % 50 + 3, i % 30 + 2)), polygon(box(point(i % 50, i % 30), point(i % 50 + 3, i % 30 + 2))) FROM generate_series(1, 2000) i;",
                "CREATE INDEX spb_b ON spb USING spgist (b);",
                "CREATE INDEX spb_g ON spb USING spgist (g);",
                "VACUUM ANALYZE spb;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spb WHERE b && box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Index Only Scan using spb_b on spb")],
                            &[T("        Index Cond: (b && '(12,12),(10,10)'::box)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b << box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("280")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b &< box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("400")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b && box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("70")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b &> box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1600")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b >> box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1480")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b ~= box '(13,12),(10,10)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b @> box '(11,11),(12,11.5)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("28")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b <@ box '(0,0),(20,20)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("472")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b &<| box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("736")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b <<| box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("535")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b |>> box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1130")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE b |&> box '(10,10),(12,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1331")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b FROM spb ORDER BY b <-> point '(3,3)' LIMIT 2",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Index Only Scan using spb_b on spb")],
                            &[T("        Order By: (b <-> '(3,3)'::point)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, b <-> point '(3.5,3.5)' FROM spb ORDER BY b <-> point '(3.5,3.5)', id LIMIT 2",
                    expected: Expected::Rows {
                        columns: &[Column("b", BOX), Column("?column?", FLOAT8)],
                        rows: &[
                            &[T("(5,4),(2,2)"), T("0")],
                            &[T("(6,5),(3,3)"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spb WHERE g && polygon '((10,10),(12,12),(12,10))'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Bitmap Heap Scan on spb")],
                            &[T("        Recheck Cond: (g && '((10,10),(12,12),(12,10))'::polygon)")],
                            &[T("        ->  Bitmap Index Scan on spb_g")],
                            &[T("              Index Cond: (g && '((10,10),(12,12),(12,10))'::polygon)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE g && polygon '((10,10),(12,12),(12,10))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("70")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE g @> polygon '((10,10),(10.5,10.5),(10.5,10))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("28")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE g <@ polygon '((0,0),(0,20),(20,20),(20,0))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("472")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE g << polygon '((10,10),(12,12),(12,10))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("280")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spb WHERE g |&> polygon '((10,10),(12,12),(12,10))'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1331")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT g FROM spb ORDER BY g <-> point '(3,3)' LIMIT 2",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Index Scan using spb_g on spb")],
                            &[T("        Order By: (g <-> '(3,3)'::point)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g <-> point '(3.5,3.5)' FROM spb ORDER BY g <-> point '(3.5,3.5)', id LIMIT 2",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("?column?", FLOAT8)],
                        rows: &[
                            &[T("2"), T("0")],
                            &[T("3"), T("0")],
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
fn test_spgist_ranges_and_inets() {
    run_scripts(&[
        ScriptTest {
            name: "range_ops and inet_ops find ranges and addresses by every strategy, with an INCLUDE column in an index-only scan",
            set_up_script: &[
                "CREATE TABLE spr (id INT PRIMARY KEY, r INT4RANGE);",
                "INSERT INTO spr SELECT i, int4range(i % 100, i % 100 + i % 7) FROM generate_series(1, 2000) i;",
                "CREATE INDEX spr_r ON spr USING spgist (r);",
                "CREATE TABLE spi (id INT PRIMARY KEY, ip INET);",
                "INSERT INTO spi SELECT i, ('10.' || (i % 4) || '.' || (i % 256) || '.1')::INET FROM generate_series(1, 2000) i;",
                "CREATE INDEX spi_ip ON spi USING spgist (ip) INCLUDE (id);",
                "VACUUM ANALYZE spr, spi;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spr WHERE r @> 15",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Index Only Scan using spr_r on spr")],
                            &[T("        Index Cond: (r @> 15)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r @> 15",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("60")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r << '[10,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("130")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r &< '[10,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("164")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r && '[10,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("77")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r &> '[10,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1542")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r >> '[10,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1508")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r -|- '[10,13)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("34")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r @> '[10,12)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("43")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r <@ '[10,20)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("413")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spr WHERE r = '[10,13)'",
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
                    query: "SELECT count(*) FROM spr WHERE r = 'empty'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("285")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT ip, id FROM spi WHERE ip << '10.1.0.0/16'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using spi_ip on spi")],
                            &[T("  Index Cond: (ip << '10.1.0.0/16'::inet)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spi WHERE ip && '10.2.3.0/24'",
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
                    query: "SELECT count(*) FROM spi WHERE ip = '10.2.3.1'",
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
                    query: "SELECT count(*) FROM spi WHERE ip <> '10.2.3.1'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spi WHERE ip < '10.1.0.0'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spi WHERE ip <= '10.1.0.1'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spi WHERE ip > '10.2.255.1'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spi WHERE ip >= '10.2.255.1'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spi WHERE ip << '10.1.0.0/16'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spi WHERE ip <<= '10.1.0.0/16'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spi WHERE ip >> '10.1.2.0/24'",
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
                    query: "SELECT count(*) FROM spi WHERE ip >>= '10.1.2.1/32'",
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
                    query: "SELECT sum(id) FROM spi WHERE ip << '10.3.0.0/16'",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T("500500")],
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
fn test_spgist_access_method() {
    run_scripts(&[
        ScriptTest {
            name: "SP-GiST's properties, single-column limit, fillfactor, exclusion constraints, writes, and concurrent builds",
            set_up_script: &[
                "CREATE TABLE spq (id INT PRIMARY KEY, p POINT, b BOX, g POLYGON, r INT4RANGE);",
                "CREATE INDEX spq_p ON spq USING spgist (p);",
                "CREATE INDEX spq_g ON spq USING spgist (g);",
                "CREATE INDEX spq_r ON spq USING spgist (r) INCLUDE (id);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_index_column_has_property('spq_p'::regclass, 1, 'distance_orderable'), pg_index_column_has_property('spq_p'::regclass, 1, 'returnable'), pg_index_column_has_property('spq_g'::regclass, 1, 'returnable'), pg_index_column_has_property('spq_g'::regclass, 1, 'distance_orderable')",
                    expected: Expected::Rows {
                        columns: &[Column("pg_index_column_has_property", BOOL), Column("pg_index_column_has_property", BOOL), Column("pg_index_column_has_property", BOOL), Column("pg_index_column_has_property", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_index_column_has_property('spq_r'::regclass, 1, 'distance_orderable'), pg_index_column_has_property('spq_r'::regclass, 2, 'returnable'), pg_index_column_has_property('spq_r'::regclass, 1, 'orderable'), pg_index_column_has_property('spq_r'::regclass, 1, 'search_nulls')",
                    expected: Expected::Rows {
                        columns: &[Column("pg_index_column_has_property", BOOL), Column("pg_index_column_has_property", BOOL), Column("pg_index_column_has_property", BOOL), Column("pg_index_column_has_property", BOOL)],
                        rows: &[
                            &[T("f"), T("t"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_indexam_has_property((SELECT oid FROM pg_am WHERE amname = 'spgist'), 'can_multi_col'), pg_indexam_has_property((SELECT oid FROM pg_am WHERE amname = 'spgist'), 'can_include'), pg_index_has_property('spq_p'::regclass, 'index_scan'), pg_index_has_property('spq_p'::regclass, 'bitmap_scan')",
                    expected: Expected::Rows {
                        columns: &[Column("pg_indexam_has_property", BOOL), Column("pg_indexam_has_property", BOOL), Column("pg_index_has_property", BOOL), Column("pg_index_has_property", BOOL)],
                        rows: &[
                            &[T("f"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX spq_two ON spq USING spgist (b, g)",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "access method \"spgist\" does not support multicolumn indexes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX spq_ff ON spq USING spgist (b) WITH (fillfactor = 50)",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX spq_bad ON spq USING spgist (b) WITH (fillfactor = 5)",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "value 5 out of bounds for option \"fillfactor\"", detail: "Valid values are between \"10\" and \"100\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE spx (r INT4RANGE, EXCLUDE USING spgist (r WITH &&))",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO spx VALUES ('[1,5)')",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO spx VALUES ('[3,7)')",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"spx_r_excl\"", detail: "Key (r)=([3,7)) conflicts with existing key (r)=([1,5)).", schema: "public", table: "spx", constraint: "spx_r_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO spx VALUES ('[5,7)')",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE spn (id INT, p POINT)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO spn SELECT i % 10, point(i % 7, i % 5) FROM generate_series(1, 300) i",
                    expected: Expected::Tag("INSERT 0 300"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX spn_p ON spn USING spgist (p)",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM spn WHERE id = 3",
                    expected: Expected::Tag("DELETE 30"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE spn SET p = point(100, 100) WHERE id = 4",
                    expected: Expected::Tag("UPDATE 30"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spn WHERE p <@ box '(0,0),(3,3)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("120")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spn WHERE p ~= '(100,100)'",
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
                    query: "CREATE TABLE spd (id INT PRIMARY KEY DEFERRABLE, p POINT)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO spd SELECT i, point(i % 7, i % 5) FROM generate_series(1, 300) i",
                    expected: Expected::Tag("INSERT 0 300"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX CONCURRENTLY spd_p ON spd USING spgist (p)",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE spd SET p = point(100, 100) WHERE id < 10",
                    expected: Expected::Tag("UPDATE 9"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spd WHERE p ~= '(100,100)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spd WHERE p <@ box '(0,0),(3,3)'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("132")],
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
fn test_spgist_text() {
    run_scripts(&[
        ScriptTest {
            name: "text_ops finds text by every strategy under a C collation, values longer than its keys' first bytes and too long for a node included",
            set_up_script: &[
                "CREATE TABLE spt (id INT PRIMARY KEY, t TEXT COLLATE \"C\");",
                "INSERT INTO spt SELECT i, 'w' || (i % 300) || repeat('x', i % 5) FROM generate_series(1, 3000) i;",
                "INSERT INTO spt VALUES (3001, NULL), (3002, ''), (3003, repeat('w1', 200)), (3004, repeat('w1', 200) || 'z'), (3005, repeat(md5('5'), 1000));",
                "CREATE INDEX spt_t ON spt USING spgist (t);",
                "VACUUM ANALYZE spt;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spt WHERE t = 'w12xx'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Index Only Scan using spt_t on spt")],
                            &[T("        Index Cond: (t = 'w12xx'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t = 'w12xx'",
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
                    query: "SELECT count(*) FROM spt WHERE t ~<~ 'w12'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("232")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t ~<=~ 'w12'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("232")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t ~>=~ 'w298'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("810")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t ~>~ 'w298'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("810")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t < 'w12'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("232")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t <= 'w12'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("232")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t >= 'w298'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("810")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t > 'w298'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("810")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t ^@ 'w29'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("110")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE starts_with(t, 'w1')",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1112")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t = repeat('w1', 200)",
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
                    query: "SELECT count(*) FROM spt WHERE t ^@ repeat('w1', 150)",
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
                    query: "SELECT count(*) FROM spt WHERE t > repeat('w1', 200)",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1891")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t < repeat('w1', 200) || 'z'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1113")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spt WHERE t IS NULL",
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
                    query: "SELECT count(*) FROM spt WHERE t = ''",
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
                    query: "EXPLAIN (COSTS OFF) SELECT t FROM spt WHERE t ^@ 'w299'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using spt_t on spt")],
                            &[T("  Index Cond: (t ^@ 'w299'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t FROM spt WHERE t ^@ 'w299' ORDER BY t",
                    expected: Expected::Rows {
                        columns: &[Column("t", TEXT)],
                        rows: &[
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                            &[T("w299xxxx")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, length(t), md5(t) FROM spt WHERE t = repeat(md5('5'), 1000)",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("length", INT4), Column("md5", TEXT)],
                        rows: &[
                            &[T("3005"), T("32000"), T("b388aaceb4e52cd1f4801b47c2d5d744")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, length(t) FROM spt WHERE t ^@ md5('5') ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("3005"), T("32000")],
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
fn test_spgist_text_collations() {
    run_scripts(&[
        ScriptTest {
            name: "text_ops prunes by bytes only under a collation that orders by them, and an INCLUDE column comes back from an index-only scan",
            set_up_script: &[
                "CREATE TABLE spe (id INT PRIMARY KEY, t TEXT COLLATE \"en-x-icu\", z TEXT COLLATE pg_c_utf8);",
                "INSERT INTO spe SELECT i, CASE WHEN i % 2 = 0 THEN 'W' ELSE 'w' END || (i % 300), 'w' || (i % 300) FROM generate_series(1, 3000) i;",
                "CREATE INDEX spe_t ON spe USING spgist (t);",
                "CREATE INDEX spe_z ON spe USING spgist (z) INCLUDE (id);",
                "VACUUM ANALYZE spe;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spe WHERE t < 'w12'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("240")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spe WHERE t >= 'W298'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("790")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spe WHERE t ~<~ 'w12'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1620")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spe WHERE t = 'W12'",
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
                    query: "SELECT count(*) FROM spe WHERE t ^@ 'W1'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("550")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM spe WHERE t < 'w12'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Index Only Scan using spe_t on spe")],
                            &[T("        Index Cond: (t < 'w12'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spe WHERE z < 'w12'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("240")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM spe WHERE z >= 'w298'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("790")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT z, id FROM spe WHERE z ^@ 'w299'",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using spe_z on spe")],
                            &[T("  Index Cond: (z ^@ 'w299'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT z, id FROM spe WHERE z ^@ 'w299' ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("z", TEXT), Column("id", INT4)],
                        rows: &[
                            &[T("w299"), T("299")],
                            &[T("w299"), T("599")],
                            &[T("w299"), T("899")],
                            &[T("w299"), T("1199")],
                            &[T("w299"), T("1499")],
                            &[T("w299"), T("1799")],
                            &[T("w299"), T("2099")],
                            &[T("w299"), T("2399")],
                            &[T("w299"), T("2699")],
                            &[T("w299"), T("2999")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_index_column_has_property('spe_t'::regclass, 1, 'returnable'), pg_index_column_has_property('spe_t'::regclass, 1, 'distance_orderable')",
                    expected: Expected::Rows {
                        columns: &[Column("pg_index_column_has_property", BOOL), Column("pg_index_column_has_property", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
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
fn test_spgist_text_nondeterministic() {
    run_scripts(&[
        ScriptTest {
            name: "text_ops under a nondeterministic collation finds the rows the collation makes equal",
            set_up_script: &[
                "CREATE COLLATION spt_ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);",
                "CREATE TABLE spci (t TEXT COLLATE spt_ci);",
                "INSERT INTO spci VALUES ('ABC'), ('abc'), ('abd');",
                "CREATE INDEX spci_t ON spci USING spgist (t);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t FROM spci WHERE t = 'abc' ORDER BY t COLLATE \"C\"",
                    expected: Expected::Rows {
                        columns: &[Column("t", TEXT)],
                        rows: &[
                            &[T("abc")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Doltgrust's text_ops index checks = under a nondeterministic collation with the collation, returning the sequential scan's ABC and abc, where PG18's compares bytes and drops ABC"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t FROM spci WHERE t < 'abd' ORDER BY t COLLATE \"C\"",
                    expected: Expected::Rows {
                        columns: &[Column("t", TEXT)],
                        rows: &[
                            &[T("ABC")],
                            &[T("abc")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = on",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t FROM spci WHERE t = 'abc' ORDER BY t COLLATE \"C\"",
                    expected: Expected::Rows {
                        columns: &[Column("t", TEXT)],
                        rows: &[
                            &[T("ABC")],
                            &[T("abc")],
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
