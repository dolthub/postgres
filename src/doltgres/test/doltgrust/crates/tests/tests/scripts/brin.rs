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
use harness::wire::{Datum, F, Fields, Receive, Send, Step, W, WireTest, run_wire_tests};


#[test]
fn test_brin() {
    run_scripts(&[
        ScriptTest {
            name: "BRIN minmax indexes find rows by comparisons and NULL tests, and keep up with inserts and updates",
            set_up_script: &[
                "CREATE TABLE bt (id INT PRIMARY KEY, a INT, b TEXT COLLATE \"C\");",
                "INSERT INTO bt SELECT i, i / 10, 'x' || (i % 100) FROM generate_series(1, 20000) i;",
                "INSERT INTO bt VALUES (20001, NULL, NULL);",
                "CREATE INDEX bt_a ON bt USING brin (a) WITH (pages_per_range = 2);",
                "CREATE INDEX bt_b ON bt USING brin (b);",
                "VACUUM ANALYZE bt;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM bt WHERE a = 500;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Bitmap Heap Scan on bt")],
                            &[T("        Recheck Cond: (a = 500)")],
                            &[T("        ->  Bitmap Index Scan on bt_a")],
                            &[T("              Index Cond: (a = 500)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE a = 500;",
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
                    query: "SELECT count(*) FROM bt WHERE a < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("29")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE a >= 1999;",
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
                    query: "SELECT count(*) FROM bt WHERE a IS NULL;",
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
                    query: "SELECT count(*) FROM bt WHERE a IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE b = 'x7';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE b > 'x98';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bt VALUES (5, 99999, 'zz') ON CONFLICT (id) DO UPDATE SET a = 99999;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bt WHERE a = 99999;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bt VALUES (30000, 77777, 'q');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bt WHERE a = 77777;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("30000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE bt SET a = -5 WHERE id = 10000;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bt WHERE a = -5;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("10000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE a BETWEEN 100 AND 200;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1010")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bt WHERE a = 500 AND b = 'x7';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on bt")],
                            &[T("  Recheck Cond: (a = 500)")],
                            &[T("  Filter: (b = 'x7'::text)")],
                            &[T("  ->  Bitmap Index Scan on bt_a")],
                            &[T("        Index Cond: (a = 500)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE a = 500 AND b = 'x0';",
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
                    query: "DROP TABLE bt;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BRIN inclusion indexes of boxes, inet, and ranges answer each of their classes' strategies",
            set_up_script: &[
                "CREATE TABLE bi (id INT PRIMARY KEY, b BOX, n INET, r INT4RANGE);",
                "INSERT INTO bi SELECT i, box(point(i, i), point(i + 1, i + 2)), ('10.' || (i / 256) % 256 || '.' || i % 256 || '.1')::inet, int4range(i, i + 5) FROM generate_series(1, 5000) i;",
                "INSERT INTO bi VALUES (5001, NULL, '::1', 'empty'), (5002, '(0,0),(0,0)', '2001:db8::1', NULL);",
                "CREATE INDEX bi_b ON bi USING brin (b) WITH (pages_per_range = 1);",
                "CREATE INDEX bi_n ON bi USING brin (n inet_inclusion_ops) WITH (pages_per_range = 1);",
                "CREATE INDEX bi_r ON bi USING brin (r) WITH (pages_per_range = 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM bi WHERE b && '(100,100),(101,101)';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on bi")],
                            &[T("  Recheck Cond: (b && '(101,101),(100,100)'::box)")],
                            &[T("  ->  Bitmap Index Scan on bi_b")],
                            &[T("        Index Cond: (b && '(101,101),(100,100)'::box)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE b && '(100,100),(101,101)' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("99")],
                            &[T("100")],
                            &[T("101")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE b << '(10,10),(11,11)';",
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
                    query: "SELECT count(*) FROM bi WHERE b &< '(10,10),(11,11)';",
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
                    query: "SELECT count(*) FROM bi WHERE b >> '(4990,0),(4991,1)';",
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
                    query: "SELECT count(*) FROM bi WHERE b &> '(4990,0),(4991,1)';",
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
                    query: "SELECT count(*) FROM bi WHERE b <<| '(0,10),(1,11)';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE b &<| '(0,10),(1,11)';",
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
                    query: "SELECT count(*) FROM bi WHERE b |>> '(0,4990),(1,4991)';",
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
                    query: "SELECT count(*) FROM bi WHERE b |&> '(0,4990),(1,4991)';",
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
                    query: "SELECT id FROM bi WHERE b @> '(50.5,51),(50.6,51.5)'::box ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE b <@ '(0,0),(3,4)'::box ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("5002")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE b ~= '(7,7),(8,9)'::box ORDER BY id;",
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
                    query: "SELECT id FROM bi WHERE b @> point(30.5, 31.5) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM bi WHERE n << '10.0.5.0/24';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on bi")],
                            &[T("  Recheck Cond: (n << '10.0.5.0/24'::inet)")],
                            &[T("  ->  Bitmap Index Scan on bi_n")],
                            &[T("        Index Cond: (n << '10.0.5.0/24'::inet)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE n << '10.0.5.0/24' ORDER BY id LIMIT 5;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE n <<= '10.1.0.0/16';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("256")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE n >> '10.0.7.1/32';",
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
                    query: "SELECT count(*) FROM bi WHERE n >>= '10.0.7.1/32';",
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
                    query: "SELECT count(*) FROM bi WHERE n && '10.2.0.0/16';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("256")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE n = '2001:db8::1';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5002")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE n < '10.0.3.0';",
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
                    query: "SELECT count(*) FROM bi WHERE n >= '10.19.0.0';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("139")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE n > '10.19.0.0';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("139")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE n <= '10.0.3.0';",
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
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM bi WHERE r @> 2500;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on bi")],
                            &[T("  Recheck Cond: (r @> 2500)")],
                            &[T("  ->  Bitmap Index Scan on bi_r")],
                            &[T("        Index Cond: (r @> 2500)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE r @> 2500 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2496")],
                            &[T("2497")],
                            &[T("2498")],
                            &[T("2499")],
                            &[T("2500")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE r && int4range(100, 103) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("96")],
                            &[T("97")],
                            &[T("98")],
                            &[T("99")],
                            &[T("100")],
                            &[T("101")],
                            &[T("102")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE r <@ int4range(10, 17) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("11")],
                            &[T("12")],
                            &[T("5001")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE r = 'empty' ORDER BY id;",
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
                    query: "SELECT id FROM bi WHERE r <@ int4range(-5, -1) ORDER BY id;",
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
                    query: "SELECT id FROM bi WHERE r -|- int4range(4000, 4003) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3995")],
                            &[T("4003")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE r << int4range(30, 31);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("25")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE r >> int4range(4980, 4981);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE r &< int4range(30, 31);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("26")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE r &> int4range(4980, 4981);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("21")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE r < int4range(30, 31);",
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
                    query: "SELECT count(*) FROM bi WHERE r <= int4range(30, 31);",
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
                    query: "SELECT count(*) FROM bi WHERE r > int4range(4980, 4981);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("21")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE r >= int4range(4980, 4981);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("21")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bi WHERE r IS NULL;",
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
                    query: "SELECT count(*) FROM bi WHERE b IS NULL;",
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
                    query: "INSERT INTO bi VALUES (6000, '(9000,9000),(9001,9001)', '192.168.0.1', int4range(9000, 9001));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bi WHERE b && '(9000,9000),(9001,9001)';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("6000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BRIN bloom indexes find rows by equality, with their classes' options and errors",
            set_up_script: &[
                "CREATE TABLE bb (id INT PRIMARY KEY, a INT, t TEXT, u UUID);",
                "INSERT INTO bb SELECT i, i % 997, md5(i::text), md5((i % 50)::text)::uuid FROM generate_series(1, 5000) i;",
                "CREATE INDEX bb_a ON bb USING brin (a int4_bloom_ops) WITH (pages_per_range = 2);",
                "CREATE INDEX bb_t ON bb USING brin (t text_bloom_ops(false_positive_rate = 0.05, n_distinct_per_range = 100));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX bb_u ON bb USING brin (u uuid_bloom_ops(n_distinct_per_range = -0.5));",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "the bloom filter is too large (22315 > 8144)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attrelid::regclass, attoptions FROM pg_attribute WHERE attrelid IN ('bb_t'::regclass, 'bb_a'::regclass) ORDER BY 1::text;",
                    expected: Expected::Rows {
                        columns: &[Column("attrelid", REGCLASS), Column("attoptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("bb_a"), Null],
                            &[T("bb_t"), T("{false_positive_rate=0.05,n_distinct_per_range=100}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM bb WHERE a = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on bb")],
                            &[T("  Recheck Cond: (a = 5)")],
                            &[T("  ->  Bitmap Index Scan on bb_a")],
                            &[T("        Index Cond: (a = 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bb WHERE a = 5 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("1002")],
                            &[T("1999")],
                            &[T("2996")],
                            &[T("3993")],
                            &[T("4990")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bb WHERE t = md5('77');",
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
                    query: "SELECT count(*) FROM bb WHERE u = md5('7')::uuid;",
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
                    query: "SELECT count(*) FROM bb WHERE a IS NULL;",
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
                    query: "INSERT INTO bb VALUES (9000, 12345, 'zz', NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bb WHERE a = 12345;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("9000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bb WHERE t = 'zz';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("9000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bb WHERE u IS NULL;",
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
                    query: "CREATE INDEX bb_bad ON bb USING brin (a int4_bloom_ops(false_positive_rate = 0.5));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "value 0.5 out of bounds for option \"false_positive_rate\"", detail: "Valid values are between \"0.000100\" and \"0.250000\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bb_bad ON bb USING brin (a int4_bloom_ops(n_distinct_per_range = -2));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "value -2 out of bounds for option \"n_distinct_per_range\"", detail: "Valid values are between \"-1.000000\" and \"2147483647.000000\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bb_bad ON bb USING brin (a int4_bloom_ops(foo = 1));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized parameter \"foo\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bb_bad ON bb USING brin (a int4_minmax_ops(n_distinct_per_range = 5));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "operator class int4_minmax_ops has no options", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bb_big ON bb USING brin (a int4_bloom_ops(false_positive_rate = 0.0001)) WITH (pages_per_range = 100);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE bbe (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bbe_big ON bbe USING brin (a int4_bloom_ops(false_positive_rate = 0.0001)) WITH (pages_per_range = 100);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bbe VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bb WHERE a < 5;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("29")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE bb, bbe;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BRIN minmax-multi indexes over each built-in class, with outliers, NaN, and their option's errors",
            set_up_script: &[
                "CREATE TABLE bm (id INT PRIMARY KEY, a INT, f FLOAT8, n NUMERIC, ts TIMESTAMPTZ, u UUID, ip INET, i INTERVAL, t TIME, tz TIMETZ, d DATE, m MACADDR, m8 MACADDR8, l PG_LSN, r REAL, s SMALLINT, b BIGINT, o OID, ti TID);",
                "INSERT INTO bm SELECT i, CASE WHEN i % 100 = 0 THEN i * 1000 ELSE i END, i / 7.0, i * 1.5, '2020-01-01'::timestamptz + i * interval '1 hour', md5(i::text)::uuid, ('10.0.' || (i / 256) % 256 || '.' || i % 256)::inet, i * interval '1 minute', '00:00'::time + i * interval '1 second', ('00:00+0' || (i % 5))::timetz + i * interval '1 second', '2020-01-01'::date + i, ('08:00:2b:01:' || lpad(to_hex((i / 256) % 256), 2, '0') || ':' || lpad(to_hex(i % 256), 2, '0'))::macaddr, ('08:00:2b:01:02:03:' || lpad(to_hex((i / 256) % 256), 2, '0') || ':' || lpad(to_hex(i % 256), 2, '0'))::macaddr8, ('0/' || to_hex(i * 16))::pg_lsn, i / 3.0, i % 30000, i::bigint * 1000000, i::oid, ('(' || i / 10 || ',' || i % 10 || ')')::tid FROM generate_series(1, 6000) i;",
                "INSERT INTO bm (id) VALUES (6001);",
                "CREATE INDEX bm_multi ON bm USING brin (a int4_minmax_multi_ops, f float8_minmax_multi_ops, n numeric_minmax_multi_ops, ts timestamptz_minmax_multi_ops, u uuid_minmax_multi_ops, ip inet_minmax_multi_ops, i interval_minmax_multi_ops) WITH (pages_per_range = 2);",
                "CREATE INDEX bm_multi2 ON bm USING brin (t time_minmax_multi_ops, tz timetz_minmax_multi_ops, d date_minmax_multi_ops, m macaddr_minmax_multi_ops, m8 macaddr8_minmax_multi_ops, l pg_lsn_minmax_multi_ops, r float4_minmax_multi_ops, s int2_minmax_multi_ops, b int8_minmax_multi_ops, o oid_minmax_multi_ops, ti tid_minmax_multi_ops);",
                "CREATE INDEX bm_a8 ON bm USING brin (a int4_minmax_multi_ops(values_per_range = 8));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM bm WHERE a = 500;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on bm")],
                            &[T("  Recheck Cond: (a = 500)")],
                            &[T("  ->  Bitmap Index Scan on bm_a8")],
                            &[T("        Index Cond: (a = 500)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bm WHERE a = 500;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bm WHERE a = 100000 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE a > 5000 AND a < 5100;",
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
                    query: "SELECT count(*) FROM bm WHERE a < 3 OR a >= 5999000;",
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
                    query: "SELECT count(*) FROM bm WHERE a BETWEEN 2000 AND 2005;",
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
                    query: "SELECT count(*) FROM bm WHERE f > 800;",
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
                    query: "SELECT count(*) FROM bm WHERE n = 3000;",
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
                    query: "SELECT count(*) FROM bm WHERE ts < '2020-01-02';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("23")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE u = md5('42')::uuid;",
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
                    query: "SELECT count(*) FROM bm WHERE ip = '10.0.3.7';",
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
                    query: "SELECT count(*) FROM bm WHERE ip < '10.0.0.10';",
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
                    query: "SELECT count(*) FROM bm WHERE i > interval '99 hours';",
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
                    query: "SELECT count(*) FROM bm WHERE t = '00:01:40';",
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
                    query: "SELECT count(*) FROM bm WHERE tz > '01:00+00';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("480")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE d = '2025-01-01';",
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
                    query: "SELECT count(*) FROM bm WHERE m = '08:00:2b:01:02:03';",
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
                    query: "SELECT count(*) FROM bm WHERE m8 >= '08:00:2b:01:02:03:17:00';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("113")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE l > '0/10000';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1904")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE r < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("29")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE s = 77;",
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
                    query: "SELECT count(*) FROM bm WHERE b > 5990000000;",
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
                    query: "SELECT count(*) FROM bm WHERE o < 20;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("19")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE ti = '(5,5)';",
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
                    query: "SELECT count(*) FROM bm WHERE a IS NULL;",
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
                    query: "INSERT INTO bm (id, a, f) SELECT i, -i, 'NaN' FROM generate_series(7000, 7100) i;",
                    expected: Expected::Tag("INSERT 0 101"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE a < -7050;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("50")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE f = 'NaN';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE a = 500;",
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
                    query: "UPDATE bm SET a = 123456789 WHERE id = 10;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bm WHERE a = 123456789;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bm_bad ON bm USING brin (a int4_minmax_multi_ops(values_per_range = 7));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "value 7 out of bounds for option \"values_per_range\"", detail: "Valid values are between \"8\" and \"256\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bm_bad ON bm USING brin (a int4_minmax_multi_ops(values_per_range = 257));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "value 257 out of bounds for option \"values_per_range\"", detail: "Valid values are between \"8\" and \"256\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE bm;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "brin_summarize_new_values, brin_summarize_range, and brin_desummarize_range, their errors, and summaries that ROLLBACK does not undo",
            set_up_script: &[
                "CREATE TABLE bt (a INT PRIMARY KEY, b TEXT);",
                "CREATE INDEX bt_b ON bt USING brin (a);",
                "CREATE INDEX bt_btree ON bt (b);",
                "INSERT INTO bt SELECT g, g::text FROM generate_series(1, 2000) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bt_btree');",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "\"bt_btree\" is not a BRIN index", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bt');",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "\"bt\" is not an index", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values(0);",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "could not open relation with OID 0", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_range('bt_b', -1);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "block number out of range: -1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_range('bt_b', 4294967295);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_range", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_range('bt_b', 4294967296);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "block number out of range: 4294967296", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt_b', 4294967295);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "block number out of range: 4294967295", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt_b', 4294967294);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_desummarize_range", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bt_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_range('bt_b', 0);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_range", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt_b', 0);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_desummarize_range", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt_b', 0) IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_range('bt_b', 100000);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_range", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE a BETWEEN 10 AND 20;",
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
                    query: "SELECT brin_summarize_range('bt_b', 5);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_range", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bt_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE a BETWEEN 10 AND 20;",
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
                    query: "INSERT INTO bt VALUES (5000, 'x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt WHERE a = 5000;",
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
                    query: "SELECT brin_summarize_new_values('bt_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE brin_nobody;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE brin_nobody;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bt_b');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of index bt_b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt_b', 0);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of index bt_b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE brin_nobody;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN READ ONLY;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt_b', 0);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_desummarize_range", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bt_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("1")],
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt_b', 0);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_desummarize_range", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "SELECT brin_summarize_new_values('bt_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("1")],
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
                    query: "SELECT brin_summarize_new_values('bt_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt_b', 0);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_desummarize_range", VOID)],
                        rows: &[
                            &[T("")],
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
                    query: "SELECT brin_summarize_new_values('bt_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("1")],
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
                    query: "CREATE TABLE bt2 (a INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bt2 SELECT generate_series(1, 100);",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bt2_b ON bt2 USING brin (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bt2_b', 0);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_desummarize_range", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bt2_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bt2 WHERE a = 7;",
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
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX CONCURRENTLY bt_c ON bt USING brin (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bt_c');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE bt;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BRIN pages_per_range and autosummarize options, and summaries that keep the build's pages_per_range until REINDEX",
            set_up_script: &[
                "CREATE TABLE ba (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX ba_b ON ba USING brin (a) WITH (autosummarize = on, pages_per_range = 4);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reloptions FROM pg_class WHERE relname = 'ba_b';",
                    expected: Expected::Rows {
                        columns: &[Column("reloptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("{autosummarize=on,pages_per_range=4}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX ba_b SET (autosummarize = off);",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reloptions FROM pg_class WHERE relname = 'ba_b';",
                    expected: Expected::Rows {
                        columns: &[Column("reloptions", TEXT_ARRAY)],
                        rows: &[
                            &[T("{pages_per_range=4,autosummarize=off}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ba_c ON ba USING brin (a) WITH (autosummarize = maybe);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid value for boolean option \"autosummarize\": maybe", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ba_d ON ba USING brin (a) WITH (pages_per_range = 0);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "value 0 out of bounds for option \"pages_per_range\"", detail: "Valid values are between \"1\" and \"131072\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ba_e ON ba USING brin (a) WITH (pages_per_range = 131073);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "value 131073 out of bounds for option \"pages_per_range\"", detail: "Valid values are between \"1\" and \"131072\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX ba_b SET (pages_per_range = 8);",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ba SELECT generate_series(1, 10);",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('ba_b');",
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ba;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE bp (a INT, pad TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bp SELECT g, repeat('x', 500) FROM generate_series(1, 400) g;",
                    expected: Expected::Tag("INSERT 0 400"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bp_b ON bp USING brin (a) WITH (pages_per_range = 2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX bp_b SET (pages_per_range = 1000);",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('bp') / 8192 > 4 AS several_pages;",
                    expected: Expected::Rows {
                        columns: &[Column("several_pages", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(brin_desummarize_range('bp_b', b)) > 0 FROM generate_series(0, pg_relation_size('bp') / 8192 - 1) b;",
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
                    query: "SELECT brin_summarize_new_values('bp_b') = ceil(pg_relation_size('bp') / 8192 / 2.0) AS by_build_value;",
                    expected: Expected::Rows {
                        columns: &[Column("by_build_value", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX bp_b;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(brin_desummarize_range('bp_b', b)) > 0 FROM generate_series(0, pg_relation_size('bp') / 8192 - 1) b;",
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
                    query: "SELECT brin_summarize_new_values('bp_b') AS after_reindex;",
                    expected: Expected::Rows {
                        columns: &[Column("after_reindex", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE bp;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "BRIN summaries run index expressions as the table's owner",
            set_up_script: &[
                "CREATE ROLE brin_owner;",
                "CREATE TABLE bo (a INT);",
                "INSERT INTO bo VALUES (1), (2);",
                "CREATE FUNCTION bo_f(x INT) RETURNS INT IMMUTABLE LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'bo_f(%) as %', x, current_user; RETURN x; END $$;",
                "ALTER TABLE bo OWNER TO brin_owner;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX bo_b ON bo USING brin (bo_f(a));",
                    notices: &[Diagnostic { code: "00000", message: "bo_f(1) as brin_owner", ..N }, Diagnostic { code: "00000", message: "bo_f(2) as brin_owner", ..N }],
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_desummarize_range('bo_b', 0);",
                    expected: Expected::Rows {
                        columns: &[Column("brin_desummarize_range", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bo_b');",
                    notices: &[Diagnostic { code: "00000", message: "bo_f(1) as brin_owner", ..N }, Diagnostic { code: "00000", message: "bo_f(2) as brin_owner", ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("brin_summarize_new_values", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE bo;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION bo_f;",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE brin_owner;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_brin_summary_types_accept_no_input() {
    run_scripts(&[
        ScriptTest {
            name: "the BRIN summary types refuse text and binary input",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'x'::pg_brin_bloom_summary;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot accept a value of type pg_brin_bloom_summary", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'x'::pg_brin_minmax_multi_summary;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot accept a value of type brin_minmax_multi_summary", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_brin_summary_types_accept_no_binary_input() {
    run_wire_tests(&[
        WireTest {
            name: "pg_brin_bloom_summary refuses binary input",
            steps: &[
                Step::Send(&[
                    Send::Parse { name: "s1", query: "SELECT $1::pg_brin_bloom_summary;", parameter_oids: &[] },
                    Send::Bind { portal: "", statement: "s1", parameter_formats: &[1], parameters: &[Datum::Bytes(&[1, 2, 3])], result_formats: &[] },
                    Send::Execute("", 0),
                    Send::Sync,
                ]),
                Step::Receive(&[
                    Receive::ParseComplete,
                    Receive::Error(Fields { severity: "ERROR", severity_unlocalized: "ERROR", code: "0A000", message: "cannot accept a value of type pg_brin_bloom_summary", where_: "unnamed portal parameter $1", ..F }),
                    Receive::ReadyForQuery(b'I'),
                ]),
            ],
            ..W
        },
        WireTest {
            name: "pg_brin_minmax_multi_summary refuses binary input",
            steps: &[
                Step::Send(&[
                    Send::Parse { name: "s2", query: "SELECT $1::pg_brin_minmax_multi_summary;", parameter_oids: &[] },
                    Send::Bind { portal: "", statement: "s2", parameter_formats: &[1], parameters: &[Datum::Bytes(&[1, 2, 3])], result_formats: &[] },
                    Send::Execute("", 0),
                    Send::Sync,
                ]),
                Step::Receive(&[
                    Receive::ParseComplete,
                    Receive::Error(Fields { severity: "ERROR", severity_unlocalized: "ERROR", code: "0A000", message: "cannot accept a value of type brin_minmax_multi_summary", where_: "unnamed portal parameter $1", ..F }),
                    Receive::ReadyForQuery(b'I'),
                ]),
            ],
            ..W
        },
    ]);
}

#[test]
fn test_reindex_in_a_transaction_gives_later_statements_the_new_pages_per_range() {
    run_scripts(&[
        ScriptTest {
            name: "a REINDEX of a BRIN index inside a transaction gives the table's later statements the index's new pages_per_range",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE bq (a INT, pad TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bq SELECT g, repeat('x', 500) FROM generate_series(1, 400) g;",
                    expected: Expected::Tag("INSERT 0 400"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bq_b ON bq USING brin (a) WITH (pages_per_range = 2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX bq_b SET (pages_per_range = 1000);",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(brin_desummarize_range('bq_b', b)) > 0 FROM generate_series(0, pg_relation_size('bq') / 8192 - 1) b;",
                    expected: Expected::Rows { columns: &[Column("?column?", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bq_b') = ceil(pg_relation_size('bq') / 8192 / 2.0) AS by_build_value;",
                    expected: Expected::Rows { columns: &[Column("by_build_value", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX INDEX bq_b;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(brin_desummarize_range('bq_b', b)) > 0 FROM generate_series(0, pg_relation_size('bq') / 8192 - 1) b;",
                    expected: Expected::Rows { columns: &[Column("?column?", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT brin_summarize_new_values('bq_b') AS after_reindex;",
                    expected: Expected::Rows { columns: &[Column("after_reindex", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
