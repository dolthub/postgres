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
fn test_btree_start_keys_that_do_not_encode() {
    run_scripts(&[
        ScriptTest {
            name: "btree scans whose start keys do not convert to the column's type",
            set_up_script: &[
                "CREATE TABLE d (id INT PRIMARY KEY, v DATE);",
                "INSERT INTO d SELECT i, DATE '2020-01-01' + i FROM generate_series(1, 10) i;",
                "INSERT INTO d VALUES (11, NULL), (12, NULL);",
                "CREATE INDEX d_v ON d (v);",
                "CREATE TABLE dd (id INT PRIMARY KEY, v DATE);",
                "INSERT INTO dd SELECT * FROM d;",
                "CREATE INDEX dd_v ON dd (v DESC NULLS FIRST);",
                "CREATE TABLE t (id INT PRIMARY KEY, a INT, b DATE);",
                "INSERT INTO t SELECT i, i % 3, DATE '2020-01-01' + i FROM generate_series(1, 12) i;",
                "CREATE INDEX t_ab ON t (a, b);",
                "CREATE TABLE n (id INT PRIMARY KEY, v INT);",
                "INSERT INTO n SELECT i, i FROM generate_series(1, 5) i;",
                "CREATE INDEX n_v ON n (v);",
                "CREATE TABLE arr (id INT PRIMARY KEY, v INT[]);",
                "INSERT INTO arr VALUES (1, '{1}'), (2, '{1,2}'), (3, '{2}'), (4, '{2,1}'), (5, '{3}');",
                "CREATE INDEX arr_v ON arr (v);",
                "CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');",
                "CREATE TABLE e (id INT PRIMARY KEY, v mood);",
                "INSERT INTO e VALUES (1, 'sad'), (2, 'ok'), (3, 'happy'), (4, 'ok');",
                "CREATE INDEX e_v ON e (v);",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v > TIMESTAMP '2020-01-05 12:00' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("5"), T("2020-01-06")],
                            &[T("6"), T("2020-01-07")],
                            &[T("7"), T("2020-01-08")],
                            &[T("8"), T("2020-01-09")],
                            &[T("9"), T("2020-01-10")],
                            &[T("10"), T("2020-01-11")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v >= TIMESTAMP '2020-01-05 12:00' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("5"), T("2020-01-06")],
                            &[T("6"), T("2020-01-07")],
                            &[T("7"), T("2020-01-08")],
                            &[T("8"), T("2020-01-09")],
                            &[T("9"), T("2020-01-10")],
                            &[T("10"), T("2020-01-11")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v < TIMESTAMP '2020-01-05 12:00' ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("4"), T("2020-01-05")],
                            &[T("3"), T("2020-01-04")],
                            &[T("2"), T("2020-01-03")],
                            &[T("1"), T("2020-01-02")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v <= TIMESTAMP '2020-01-05 12:00' ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("4"), T("2020-01-05")],
                            &[T("3"), T("2020-01-04")],
                            &[T("2"), T("2020-01-03")],
                            &[T("1"), T("2020-01-02")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v > TIMESTAMP '2020-01-05 00:00' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("5"), T("2020-01-06")],
                            &[T("6"), T("2020-01-07")],
                            &[T("7"), T("2020-01-08")],
                            &[T("8"), T("2020-01-09")],
                            &[T("9"), T("2020-01-10")],
                            &[T("10"), T("2020-01-11")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v <= TIMESTAMP '2020-01-05 00:00' ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("4"), T("2020-01-05")],
                            &[T("3"), T("2020-01-04")],
                            &[T("2"), T("2020-01-03")],
                            &[T("1"), T("2020-01-02")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v = TIMESTAMP '2020-01-05 00:00';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("4"), T("2020-01-05")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v = ANY (ARRAY[TIMESTAMP '2020-01-03 00:00', TIMESTAMP '2020-01-07 00:00', TIMESTAMP '2020-01-08 01:00']) ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("2"), T("2020-01-03")],
                            &[T("6"), T("2020-01-07")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v = ANY (ARRAY[TIMESTAMP '2020-01-03 00:00', TIMESTAMP '2020-01-07 00:00']) ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("6"), T("2020-01-07")],
                            &[T("2"), T("2020-01-03")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dd WHERE v > TIMESTAMP '2020-01-05 12:00' ORDER BY v DESC NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("10"), T("2020-01-11")],
                            &[T("9"), T("2020-01-10")],
                            &[T("8"), T("2020-01-09")],
                            &[T("7"), T("2020-01-08")],
                            &[T("6"), T("2020-01-07")],
                            &[T("5"), T("2020-01-06")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dd WHERE v < TIMESTAMP '2020-01-05 12:00' ORDER BY v DESC NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("4"), T("2020-01-05")],
                            &[T("3"), T("2020-01-04")],
                            &[T("2"), T("2020-01-03")],
                            &[T("1"), T("2020-01-02")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dd WHERE v >= TIMESTAMP '2020-01-05 00:00' ORDER BY v DESC NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("10"), T("2020-01-11")],
                            &[T("9"), T("2020-01-10")],
                            &[T("8"), T("2020-01-09")],
                            &[T("7"), T("2020-01-08")],
                            &[T("6"), T("2020-01-07")],
                            &[T("5"), T("2020-01-06")],
                            &[T("4"), T("2020-01-05")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dd WHERE v <= TIMESTAMP '2020-01-05 00:00' ORDER BY v DESC NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("4"), T("2020-01-05")],
                            &[T("3"), T("2020-01-04")],
                            &[T("2"), T("2020-01-03")],
                            &[T("1"), T("2020-01-02")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dd WHERE v > TIMESTAMP '2020-01-05 12:00' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("5"), T("2020-01-06")],
                            &[T("6"), T("2020-01-07")],
                            &[T("7"), T("2020-01-08")],
                            &[T("8"), T("2020-01-09")],
                            &[T("9"), T("2020-01-10")],
                            &[T("10"), T("2020-01-11")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dd WHERE v < TIMESTAMP '2020-01-05 12:00' ORDER BY v NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("1"), T("2020-01-02")],
                            &[T("2"), T("2020-01-03")],
                            &[T("3"), T("2020-01-04")],
                            &[T("4"), T("2020-01-05")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dd WHERE v = ANY (ARRAY[TIMESTAMP '2020-01-03 00:00', TIMESTAMP '2020-01-07 00:00']) ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("6"), T("2020-01-07")],
                            &[T("2"), T("2020-01-03")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM dd WHERE v IS NULL OR v > TIMESTAMP '2020-01-09 12:00' ORDER BY v DESC NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("11"), Null],
                            &[T("12"), Null],
                            &[T("10"), T("2020-01-11")],
                            &[T("9"), T("2020-01-10")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d WHERE v > TIMESTAMP '2020-01-05 12:00' AND v < TIMESTAMP '2020-01-08 12:00' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", DATE)],
                        rows: &[
                            &[T("5"), T("2020-01-06")],
                            &[T("6"), T("2020-01-07")],
                            &[T("7"), T("2020-01-08")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM t WHERE a = 1 AND b > TIMESTAMP '2020-01-05 12:00' ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", DATE)],
                        rows: &[
                            &[T("7"), T("1"), T("2020-01-08")],
                            &[T("10"), T("1"), T("2020-01-11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM t WHERE a = 1 AND b < TIMESTAMP '2020-01-08 12:00' ORDER BY a DESC, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", DATE)],
                        rows: &[
                            &[T("7"), T("1"), T("2020-01-08")],
                            &[T("4"), T("1"), T("2020-01-05")],
                            &[T("1"), T("1"), T("2020-01-02")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM t WHERE a IN (0, 2) AND b > TIMESTAMP '2020-01-05 12:00' ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", DATE)],
                        rows: &[
                            &[T("6"), T("0"), T("2020-01-07")],
                            &[T("9"), T("0"), T("2020-01-10")],
                            &[T("12"), T("0"), T("2020-01-13")],
                            &[T("5"), T("2"), T("2020-01-06")],
                            &[T("8"), T("2"), T("2020-01-09")],
                            &[T("11"), T("2"), T("2020-01-12")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM t WHERE a IN (0, 2) AND b <= TIMESTAMP '2020-01-09 12:00' ORDER BY a DESC, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", DATE)],
                        rows: &[
                            &[T("8"), T("2"), T("2020-01-09")],
                            &[T("5"), T("2"), T("2020-01-06")],
                            &[T("2"), T("2"), T("2020-01-03")],
                            &[T("6"), T("0"), T("2020-01-07")],
                            &[T("3"), T("0"), T("2020-01-04")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM t WHERE b > TIMESTAMP '2020-01-09 12:00' ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", DATE)],
                        rows: &[
                            &[T("9"), T("0"), T("2020-01-10")],
                            &[T("12"), T("0"), T("2020-01-13")],
                            &[T("10"), T("1"), T("2020-01-11")],
                            &[T("11"), T("2"), T("2020-01-12")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM n WHERE v > 3000000000::INT8 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM n WHERE v < 3000000000::INT8 ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("5"), T("5")],
                            &[T("4"), T("4")],
                            &[T("3"), T("3")],
                            &[T("2"), T("2")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM n WHERE v > -3000000000::INT8 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM n WHERE v < -3000000000::INT8 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM n WHERE v > 2.5::FLOAT8 ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM arr WHERE v = '{1,2}' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4_ARRAY)],
                        rows: &[
                            &[T("2"), T("{1,2}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM arr WHERE v > '{1,2}' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4_ARRAY)],
                        rows: &[
                            &[T("3"), T("{2}")],
                            &[T("4"), T("{2,1}")],
                            &[T("5"), T("{3}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM arr WHERE v < '{2}' ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4_ARRAY)],
                        rows: &[
                            &[T("2"), T("{1,2}")],
                            &[T("1"), T("{1}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM arr WHERE v IN ('{1}', '{2,1}') ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4_ARRAY)],
                        rows: &[
                            &[T("1"), T("{1}")],
                            &[T("4"), T("{2,1}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM e WHERE v = 'ok' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("2"), T("ok")],
                            &[T("4"), T("ok")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM e WHERE v > 'sad' ORDER BY v, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("2"), T("ok")],
                            &[T("4"), T("ok")],
                            &[T("3"), T("happy")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM e WHERE v < 'happy' ORDER BY v DESC, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("2"), T("ok")],
                            &[T("4"), T("ok")],
                            &[T("1"), T("sad")],
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
fn test_btree_skip_scans() {
    run_scripts(&[
        ScriptTest {
            name: "btree skip scans over each type with skip support, and text without it",
            set_up_script: &[
                "SET TIME ZONE 'UTC';",
                "CREATE TABLE s (id INT PRIMARY KEY, a INT2, b INT4, c INT8, d OID, f \"char\", g DATE, h TIMESTAMP, i TIMESTAMPTZ, j UUID, k TEXT, e BOOL, v INT);",
                "INSERT INTO s SELECT i * 10 + w, CASE WHEN i = 6 THEN NULL ELSE i::INT2 END, CASE WHEN i = 6 THEN NULL ELSE i END, CASE WHEN i = 6 THEN NULL ELSE i::INT8 * 1000000000000 END, CASE WHEN i = 6 THEN NULL ELSE i::OID END, CASE WHEN i = 6 THEN NULL ELSE (64 + i)::\"char\" END, CASE WHEN i = 6 THEN NULL ELSE DATE '2020-01-01' + i END, CASE WHEN i = 6 THEN NULL ELSE TIMESTAMP '2020-01-01' + i * INTERVAL '1 hour' END, CASE WHEN i = 6 THEN NULL ELSE TIMESTAMPTZ '2020-01-01 00:00+00' + i * INTERVAL '1 minute' END, CASE WHEN i = 6 THEN NULL ELSE ('00000000-0000-0000-0000-0000000000' || lpad(i::TEXT, 2, '0'))::UUID END, CASE WHEN i = 6 THEN NULL ELSE 'k' || lpad(i::TEXT, 2, '0') END, i % 2 = 0, w FROM generate_series(1, 6) i, generate_series(1, 5) w;",
                "CREATE TABLE sd (LIKE s INCLUDING ALL);",
                "INSERT INTO sd SELECT * FROM s;",
                "CREATE INDEX s_a ON s (a, v);",
                "CREATE INDEX sd_a ON sd (a DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_b ON s (b, v);",
                "CREATE INDEX sd_b ON sd (b DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_c ON s (c, v);",
                "CREATE INDEX sd_c ON sd (c DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_d ON s (d, v);",
                "CREATE INDEX sd_d ON sd (d DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_f ON s (f, v);",
                "CREATE INDEX sd_f ON sd (f DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_g ON s (g, v);",
                "CREATE INDEX sd_g ON sd (g DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_h ON s (h, v);",
                "CREATE INDEX sd_h ON sd (h DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_i ON s (i, v);",
                "CREATE INDEX sd_i ON sd (i DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_j ON s (j, v);",
                "CREATE INDEX sd_j ON sd (j DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_k ON s (k, v);",
                "CREATE INDEX sd_k ON sd (k DESC NULLS FIRST, v DESC);",
                "CREATE INDEX s_e ON s (e, v, id);",
                "CREATE INDEX sd_e ON sd (e DESC NULLS FIRST, v DESC, id DESC);",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
                "ANALYZE s;",
                "ANALYZE sd;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, a, v FROM s WHERE v = 3 ORDER BY a, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("1"), T("3")],
                            &[T("23"), T("2"), T("3")],
                            &[T("33"), T("3"), T("3")],
                            &[T("43"), T("4"), T("3")],
                            &[T("53"), T("5"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, v FROM s WHERE v IN (2, 4) ORDER BY a DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("5"), T("4")],
                            &[T("52"), T("5"), T("2")],
                            &[T("44"), T("4"), T("4")],
                            &[T("42"), T("4"), T("2")],
                            &[T("34"), T("3"), T("4")],
                            &[T("32"), T("3"), T("2")],
                            &[T("24"), T("2"), T("4")],
                            &[T("22"), T("2"), T("2")],
                            &[T("14"), T("1"), T("4")],
                            &[T("12"), T("1"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, v FROM s WHERE v > 4 ORDER BY a DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("5"), T("5")],
                            &[T("45"), T("4"), T("5")],
                            &[T("35"), T("3"), T("5")],
                            &[T("25"), T("2"), T("5")],
                            &[T("15"), T("1"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, v FROM s WHERE a IS NULL AND v < 3 ORDER BY a, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM s WHERE v = 3 ORDER BY b, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("1"), T("3")],
                            &[T("23"), T("2"), T("3")],
                            &[T("33"), T("3"), T("3")],
                            &[T("43"), T("4"), T("3")],
                            &[T("53"), T("5"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM s WHERE v IN (2, 4) ORDER BY b DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("5"), T("4")],
                            &[T("52"), T("5"), T("2")],
                            &[T("44"), T("4"), T("4")],
                            &[T("42"), T("4"), T("2")],
                            &[T("34"), T("3"), T("4")],
                            &[T("32"), T("3"), T("2")],
                            &[T("24"), T("2"), T("4")],
                            &[T("22"), T("2"), T("2")],
                            &[T("14"), T("1"), T("4")],
                            &[T("12"), T("1"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM s WHERE v > 4 ORDER BY b DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("5"), T("5")],
                            &[T("45"), T("4"), T("5")],
                            &[T("35"), T("3"), T("5")],
                            &[T("25"), T("2"), T("5")],
                            &[T("15"), T("1"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM s WHERE b IS NULL AND v < 3 ORDER BY b, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, v FROM s WHERE v = 3 ORDER BY c, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("1000000000000"), T("3")],
                            &[T("23"), T("2000000000000"), T("3")],
                            &[T("33"), T("3000000000000"), T("3")],
                            &[T("43"), T("4000000000000"), T("3")],
                            &[T("53"), T("5000000000000"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, v FROM s WHERE v IN (2, 4) ORDER BY c DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("5000000000000"), T("4")],
                            &[T("52"), T("5000000000000"), T("2")],
                            &[T("44"), T("4000000000000"), T("4")],
                            &[T("42"), T("4000000000000"), T("2")],
                            &[T("34"), T("3000000000000"), T("4")],
                            &[T("32"), T("3000000000000"), T("2")],
                            &[T("24"), T("2000000000000"), T("4")],
                            &[T("22"), T("2000000000000"), T("2")],
                            &[T("14"), T("1000000000000"), T("4")],
                            &[T("12"), T("1000000000000"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, v FROM s WHERE v > 4 ORDER BY c DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("5000000000000"), T("5")],
                            &[T("45"), T("4000000000000"), T("5")],
                            &[T("35"), T("3000000000000"), T("5")],
                            &[T("25"), T("2000000000000"), T("5")],
                            &[T("15"), T("1000000000000"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, v FROM s WHERE c IS NULL AND v < 3 ORDER BY c, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, v FROM s WHERE v = 3 ORDER BY d, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", OID), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("1"), T("3")],
                            &[T("23"), T("2"), T("3")],
                            &[T("33"), T("3"), T("3")],
                            &[T("43"), T("4"), T("3")],
                            &[T("53"), T("5"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, v FROM s WHERE v IN (2, 4) ORDER BY d DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", OID), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("5"), T("4")],
                            &[T("52"), T("5"), T("2")],
                            &[T("44"), T("4"), T("4")],
                            &[T("42"), T("4"), T("2")],
                            &[T("34"), T("3"), T("4")],
                            &[T("32"), T("3"), T("2")],
                            &[T("24"), T("2"), T("4")],
                            &[T("22"), T("2"), T("2")],
                            &[T("14"), T("1"), T("4")],
                            &[T("12"), T("1"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, v FROM s WHERE v > 4 ORDER BY d DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", OID), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("5"), T("5")],
                            &[T("45"), T("4"), T("5")],
                            &[T("35"), T("3"), T("5")],
                            &[T("25"), T("2"), T("5")],
                            &[T("15"), T("1"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, v FROM s WHERE d IS NULL AND v < 3 ORDER BY d, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", OID), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, f, v FROM s WHERE v = 3 ORDER BY f, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f", CHAR), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("A"), T("3")],
                            &[T("23"), T("B"), T("3")],
                            &[T("33"), T("C"), T("3")],
                            &[T("43"), T("D"), T("3")],
                            &[T("53"), T("E"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, f, v FROM s WHERE v IN (2, 4) ORDER BY f DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f", CHAR), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("E"), T("4")],
                            &[T("52"), T("E"), T("2")],
                            &[T("44"), T("D"), T("4")],
                            &[T("42"), T("D"), T("2")],
                            &[T("34"), T("C"), T("4")],
                            &[T("32"), T("C"), T("2")],
                            &[T("24"), T("B"), T("4")],
                            &[T("22"), T("B"), T("2")],
                            &[T("14"), T("A"), T("4")],
                            &[T("12"), T("A"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, f, v FROM s WHERE v > 4 ORDER BY f DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f", CHAR), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("E"), T("5")],
                            &[T("45"), T("D"), T("5")],
                            &[T("35"), T("C"), T("5")],
                            &[T("25"), T("B"), T("5")],
                            &[T("15"), T("A"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, f, v FROM s WHERE f IS NULL AND v < 3 ORDER BY f, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f", CHAR), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM s WHERE v = 3 ORDER BY g, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("2020-01-02"), T("3")],
                            &[T("23"), T("2020-01-03"), T("3")],
                            &[T("33"), T("2020-01-04"), T("3")],
                            &[T("43"), T("2020-01-05"), T("3")],
                            &[T("53"), T("2020-01-06"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM s WHERE v IN (2, 4) ORDER BY g DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("2020-01-06"), T("4")],
                            &[T("52"), T("2020-01-06"), T("2")],
                            &[T("44"), T("2020-01-05"), T("4")],
                            &[T("42"), T("2020-01-05"), T("2")],
                            &[T("34"), T("2020-01-04"), T("4")],
                            &[T("32"), T("2020-01-04"), T("2")],
                            &[T("24"), T("2020-01-03"), T("4")],
                            &[T("22"), T("2020-01-03"), T("2")],
                            &[T("14"), T("2020-01-02"), T("4")],
                            &[T("12"), T("2020-01-02"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM s WHERE v > 4 ORDER BY g DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("2020-01-06"), T("5")],
                            &[T("45"), T("2020-01-05"), T("5")],
                            &[T("35"), T("2020-01-04"), T("5")],
                            &[T("25"), T("2020-01-03"), T("5")],
                            &[T("15"), T("2020-01-02"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM s WHERE g IS NULL AND v < 3 ORDER BY g, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, h, v FROM s WHERE v = 3 ORDER BY h, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("h", TIMESTAMP), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("2020-01-01 01:00:00"), T("3")],
                            &[T("23"), T("2020-01-01 02:00:00"), T("3")],
                            &[T("33"), T("2020-01-01 03:00:00"), T("3")],
                            &[T("43"), T("2020-01-01 04:00:00"), T("3")],
                            &[T("53"), T("2020-01-01 05:00:00"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, h, v FROM s WHERE v IN (2, 4) ORDER BY h DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("h", TIMESTAMP), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("2020-01-01 05:00:00"), T("4")],
                            &[T("52"), T("2020-01-01 05:00:00"), T("2")],
                            &[T("44"), T("2020-01-01 04:00:00"), T("4")],
                            &[T("42"), T("2020-01-01 04:00:00"), T("2")],
                            &[T("34"), T("2020-01-01 03:00:00"), T("4")],
                            &[T("32"), T("2020-01-01 03:00:00"), T("2")],
                            &[T("24"), T("2020-01-01 02:00:00"), T("4")],
                            &[T("22"), T("2020-01-01 02:00:00"), T("2")],
                            &[T("14"), T("2020-01-01 01:00:00"), T("4")],
                            &[T("12"), T("2020-01-01 01:00:00"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, h, v FROM s WHERE v > 4 ORDER BY h DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("h", TIMESTAMP), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("2020-01-01 05:00:00"), T("5")],
                            &[T("45"), T("2020-01-01 04:00:00"), T("5")],
                            &[T("35"), T("2020-01-01 03:00:00"), T("5")],
                            &[T("25"), T("2020-01-01 02:00:00"), T("5")],
                            &[T("15"), T("2020-01-01 01:00:00"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, h, v FROM s WHERE h IS NULL AND v < 3 ORDER BY h, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("h", TIMESTAMP), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, i, v FROM s WHERE v = 3 ORDER BY i, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("i", TIMESTAMPTZ), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("2020-01-01 00:01:00+00"), T("3")],
                            &[T("23"), T("2020-01-01 00:02:00+00"), T("3")],
                            &[T("33"), T("2020-01-01 00:03:00+00"), T("3")],
                            &[T("43"), T("2020-01-01 00:04:00+00"), T("3")],
                            &[T("53"), T("2020-01-01 00:05:00+00"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, i, v FROM s WHERE v IN (2, 4) ORDER BY i DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("i", TIMESTAMPTZ), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("2020-01-01 00:05:00+00"), T("4")],
                            &[T("52"), T("2020-01-01 00:05:00+00"), T("2")],
                            &[T("44"), T("2020-01-01 00:04:00+00"), T("4")],
                            &[T("42"), T("2020-01-01 00:04:00+00"), T("2")],
                            &[T("34"), T("2020-01-01 00:03:00+00"), T("4")],
                            &[T("32"), T("2020-01-01 00:03:00+00"), T("2")],
                            &[T("24"), T("2020-01-01 00:02:00+00"), T("4")],
                            &[T("22"), T("2020-01-01 00:02:00+00"), T("2")],
                            &[T("14"), T("2020-01-01 00:01:00+00"), T("4")],
                            &[T("12"), T("2020-01-01 00:01:00+00"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, i, v FROM s WHERE v > 4 ORDER BY i DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("i", TIMESTAMPTZ), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("2020-01-01 00:05:00+00"), T("5")],
                            &[T("45"), T("2020-01-01 00:04:00+00"), T("5")],
                            &[T("35"), T("2020-01-01 00:03:00+00"), T("5")],
                            &[T("25"), T("2020-01-01 00:02:00+00"), T("5")],
                            &[T("15"), T("2020-01-01 00:01:00+00"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, i, v FROM s WHERE i IS NULL AND v < 3 ORDER BY i, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("i", TIMESTAMPTZ), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, j, v FROM s WHERE v = 3 ORDER BY j, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("j", UUID), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("00000000-0000-0000-0000-000000000001"), T("3")],
                            &[T("23"), T("00000000-0000-0000-0000-000000000002"), T("3")],
                            &[T("33"), T("00000000-0000-0000-0000-000000000003"), T("3")],
                            &[T("43"), T("00000000-0000-0000-0000-000000000004"), T("3")],
                            &[T("53"), T("00000000-0000-0000-0000-000000000005"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, j, v FROM s WHERE v IN (2, 4) ORDER BY j DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("j", UUID), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("00000000-0000-0000-0000-000000000005"), T("4")],
                            &[T("52"), T("00000000-0000-0000-0000-000000000005"), T("2")],
                            &[T("44"), T("00000000-0000-0000-0000-000000000004"), T("4")],
                            &[T("42"), T("00000000-0000-0000-0000-000000000004"), T("2")],
                            &[T("34"), T("00000000-0000-0000-0000-000000000003"), T("4")],
                            &[T("32"), T("00000000-0000-0000-0000-000000000003"), T("2")],
                            &[T("24"), T("00000000-0000-0000-0000-000000000002"), T("4")],
                            &[T("22"), T("00000000-0000-0000-0000-000000000002"), T("2")],
                            &[T("14"), T("00000000-0000-0000-0000-000000000001"), T("4")],
                            &[T("12"), T("00000000-0000-0000-0000-000000000001"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, j, v FROM s WHERE v > 4 ORDER BY j DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("j", UUID), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("00000000-0000-0000-0000-000000000005"), T("5")],
                            &[T("45"), T("00000000-0000-0000-0000-000000000004"), T("5")],
                            &[T("35"), T("00000000-0000-0000-0000-000000000003"), T("5")],
                            &[T("25"), T("00000000-0000-0000-0000-000000000002"), T("5")],
                            &[T("15"), T("00000000-0000-0000-0000-000000000001"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, j, v FROM s WHERE j IS NULL AND v < 3 ORDER BY j, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("j", UUID), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM s WHERE v = 3 ORDER BY k, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("k01"), T("3")],
                            &[T("23"), T("k02"), T("3")],
                            &[T("33"), T("k03"), T("3")],
                            &[T("43"), T("k04"), T("3")],
                            &[T("53"), T("k05"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM s WHERE v IN (2, 4) ORDER BY k DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("k05"), T("4")],
                            &[T("52"), T("k05"), T("2")],
                            &[T("44"), T("k04"), T("4")],
                            &[T("42"), T("k04"), T("2")],
                            &[T("34"), T("k03"), T("4")],
                            &[T("32"), T("k03"), T("2")],
                            &[T("24"), T("k02"), T("4")],
                            &[T("22"), T("k02"), T("2")],
                            &[T("14"), T("k01"), T("4")],
                            &[T("12"), T("k01"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM s WHERE v > 4 ORDER BY k DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("k05"), T("5")],
                            &[T("45"), T("k04"), T("5")],
                            &[T("35"), T("k03"), T("5")],
                            &[T("25"), T("k02"), T("5")],
                            &[T("15"), T("k01"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM s WHERE k IS NULL AND v < 3 ORDER BY k, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, e, v FROM s WHERE v = 3 ORDER BY e, v, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("e", BOOL), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("f"), T("3")],
                            &[T("33"), T("f"), T("3")],
                            &[T("53"), T("f"), T("3")],
                            &[T("23"), T("t"), T("3")],
                            &[T("43"), T("t"), T("3")],
                            &[T("63"), T("t"), T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, e, v FROM s WHERE v IN (2, 4) ORDER BY e DESC NULLS FIRST, v DESC, id DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("e", BOOL), Column("v", INT4)],
                        rows: &[
                            &[T("64"), T("t"), T("4")],
                            &[T("44"), T("t"), T("4")],
                            &[T("24"), T("t"), T("4")],
                            &[T("62"), T("t"), T("2")],
                            &[T("42"), T("t"), T("2")],
                            &[T("22"), T("t"), T("2")],
                            &[T("54"), T("f"), T("4")],
                            &[T("34"), T("f"), T("4")],
                            &[T("14"), T("f"), T("4")],
                            &[T("52"), T("f"), T("2")],
                            &[T("32"), T("f"), T("2")],
                            &[T("12"), T("f"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, e, v FROM s WHERE v > 4 ORDER BY e DESC, v DESC, id DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("e", BOOL), Column("v", INT4)],
                        rows: &[
                            &[T("65"), T("t"), T("5")],
                            &[T("45"), T("t"), T("5")],
                            &[T("25"), T("t"), T("5")],
                            &[T("55"), T("f"), T("5")],
                            &[T("35"), T("f"), T("5")],
                            &[T("15"), T("f"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, e, v FROM s WHERE e IS NULL AND v < 3 ORDER BY e, v, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("e", BOOL), Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM s WHERE b BETWEEN 2 AND 4 AND v = 5 ORDER BY b, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("25"), T("2"), T("5")],
                            &[T("35"), T("3"), T("5")],
                            &[T("45"), T("4"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM s WHERE b > 2 AND v IN (1, 5) ORDER BY b DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("55"), T("5"), T("5")],
                            &[T("51"), T("5"), T("1")],
                            &[T("45"), T("4"), T("5")],
                            &[T("41"), T("4"), T("1")],
                            &[T("35"), T("3"), T("5")],
                            &[T("31"), T("3"), T("1")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM s WHERE b < 3 AND v >= 4 ORDER BY b NULLS FIRST, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("14"), T("1"), T("4")],
                            &[T("15"), T("1"), T("5")],
                            &[T("24"), T("2"), T("4")],
                            &[T("25"), T("2"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM s WHERE k >= 'k03' AND v = 2 ORDER BY k, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("32"), T("k03"), T("2")],
                            &[T("42"), T("k04"), T("2")],
                            &[T("52"), T("k05"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM s WHERE g > DATE '2020-01-03' AND v = 1 ORDER BY g DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("51"), T("2020-01-06"), T("1")],
                            &[T("41"), T("2020-01-05"), T("1")],
                            &[T("31"), T("2020-01-04"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM s WHERE b IS NOT NULL AND v = 3;",
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
                    query: "SELECT id, a, v FROM sd WHERE v = 3 ORDER BY a, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("1"), T("3")],
                            &[T("23"), T("2"), T("3")],
                            &[T("33"), T("3"), T("3")],
                            &[T("43"), T("4"), T("3")],
                            &[T("53"), T("5"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, v FROM sd WHERE v IN (2, 4) ORDER BY a DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("5"), T("4")],
                            &[T("52"), T("5"), T("2")],
                            &[T("44"), T("4"), T("4")],
                            &[T("42"), T("4"), T("2")],
                            &[T("34"), T("3"), T("4")],
                            &[T("32"), T("3"), T("2")],
                            &[T("24"), T("2"), T("4")],
                            &[T("22"), T("2"), T("2")],
                            &[T("14"), T("1"), T("4")],
                            &[T("12"), T("1"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, v FROM sd WHERE v > 4 ORDER BY a DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("5"), T("5")],
                            &[T("45"), T("4"), T("5")],
                            &[T("35"), T("3"), T("5")],
                            &[T("25"), T("2"), T("5")],
                            &[T("15"), T("1"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, v FROM sd WHERE a IS NULL AND v < 3 ORDER BY a, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM sd WHERE v = 3 ORDER BY b, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("1"), T("3")],
                            &[T("23"), T("2"), T("3")],
                            &[T("33"), T("3"), T("3")],
                            &[T("43"), T("4"), T("3")],
                            &[T("53"), T("5"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM sd WHERE v IN (2, 4) ORDER BY b DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("5"), T("4")],
                            &[T("52"), T("5"), T("2")],
                            &[T("44"), T("4"), T("4")],
                            &[T("42"), T("4"), T("2")],
                            &[T("34"), T("3"), T("4")],
                            &[T("32"), T("3"), T("2")],
                            &[T("24"), T("2"), T("4")],
                            &[T("22"), T("2"), T("2")],
                            &[T("14"), T("1"), T("4")],
                            &[T("12"), T("1"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM sd WHERE v > 4 ORDER BY b DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("5"), T("5")],
                            &[T("45"), T("4"), T("5")],
                            &[T("35"), T("3"), T("5")],
                            &[T("25"), T("2"), T("5")],
                            &[T("15"), T("1"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM sd WHERE b IS NULL AND v < 3 ORDER BY b, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, v FROM sd WHERE v = 3 ORDER BY c, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("1000000000000"), T("3")],
                            &[T("23"), T("2000000000000"), T("3")],
                            &[T("33"), T("3000000000000"), T("3")],
                            &[T("43"), T("4000000000000"), T("3")],
                            &[T("53"), T("5000000000000"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, v FROM sd WHERE v IN (2, 4) ORDER BY c DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("5000000000000"), T("4")],
                            &[T("52"), T("5000000000000"), T("2")],
                            &[T("44"), T("4000000000000"), T("4")],
                            &[T("42"), T("4000000000000"), T("2")],
                            &[T("34"), T("3000000000000"), T("4")],
                            &[T("32"), T("3000000000000"), T("2")],
                            &[T("24"), T("2000000000000"), T("4")],
                            &[T("22"), T("2000000000000"), T("2")],
                            &[T("14"), T("1000000000000"), T("4")],
                            &[T("12"), T("1000000000000"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, v FROM sd WHERE v > 4 ORDER BY c DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("5000000000000"), T("5")],
                            &[T("45"), T("4000000000000"), T("5")],
                            &[T("35"), T("3000000000000"), T("5")],
                            &[T("25"), T("2000000000000"), T("5")],
                            &[T("15"), T("1000000000000"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, v FROM sd WHERE c IS NULL AND v < 3 ORDER BY c, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, v FROM sd WHERE v = 3 ORDER BY d, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", OID), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("1"), T("3")],
                            &[T("23"), T("2"), T("3")],
                            &[T("33"), T("3"), T("3")],
                            &[T("43"), T("4"), T("3")],
                            &[T("53"), T("5"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, v FROM sd WHERE v IN (2, 4) ORDER BY d DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", OID), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("5"), T("4")],
                            &[T("52"), T("5"), T("2")],
                            &[T("44"), T("4"), T("4")],
                            &[T("42"), T("4"), T("2")],
                            &[T("34"), T("3"), T("4")],
                            &[T("32"), T("3"), T("2")],
                            &[T("24"), T("2"), T("4")],
                            &[T("22"), T("2"), T("2")],
                            &[T("14"), T("1"), T("4")],
                            &[T("12"), T("1"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, v FROM sd WHERE v > 4 ORDER BY d DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", OID), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("5"), T("5")],
                            &[T("45"), T("4"), T("5")],
                            &[T("35"), T("3"), T("5")],
                            &[T("25"), T("2"), T("5")],
                            &[T("15"), T("1"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, v FROM sd WHERE d IS NULL AND v < 3 ORDER BY d, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", OID), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, f, v FROM sd WHERE v = 3 ORDER BY f, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f", CHAR), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("A"), T("3")],
                            &[T("23"), T("B"), T("3")],
                            &[T("33"), T("C"), T("3")],
                            &[T("43"), T("D"), T("3")],
                            &[T("53"), T("E"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, f, v FROM sd WHERE v IN (2, 4) ORDER BY f DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f", CHAR), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("E"), T("4")],
                            &[T("52"), T("E"), T("2")],
                            &[T("44"), T("D"), T("4")],
                            &[T("42"), T("D"), T("2")],
                            &[T("34"), T("C"), T("4")],
                            &[T("32"), T("C"), T("2")],
                            &[T("24"), T("B"), T("4")],
                            &[T("22"), T("B"), T("2")],
                            &[T("14"), T("A"), T("4")],
                            &[T("12"), T("A"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, f, v FROM sd WHERE v > 4 ORDER BY f DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f", CHAR), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("E"), T("5")],
                            &[T("45"), T("D"), T("5")],
                            &[T("35"), T("C"), T("5")],
                            &[T("25"), T("B"), T("5")],
                            &[T("15"), T("A"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, f, v FROM sd WHERE f IS NULL AND v < 3 ORDER BY f, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f", CHAR), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM sd WHERE v = 3 ORDER BY g, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("2020-01-02"), T("3")],
                            &[T("23"), T("2020-01-03"), T("3")],
                            &[T("33"), T("2020-01-04"), T("3")],
                            &[T("43"), T("2020-01-05"), T("3")],
                            &[T("53"), T("2020-01-06"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM sd WHERE v IN (2, 4) ORDER BY g DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("2020-01-06"), T("4")],
                            &[T("52"), T("2020-01-06"), T("2")],
                            &[T("44"), T("2020-01-05"), T("4")],
                            &[T("42"), T("2020-01-05"), T("2")],
                            &[T("34"), T("2020-01-04"), T("4")],
                            &[T("32"), T("2020-01-04"), T("2")],
                            &[T("24"), T("2020-01-03"), T("4")],
                            &[T("22"), T("2020-01-03"), T("2")],
                            &[T("14"), T("2020-01-02"), T("4")],
                            &[T("12"), T("2020-01-02"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM sd WHERE v > 4 ORDER BY g DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("2020-01-06"), T("5")],
                            &[T("45"), T("2020-01-05"), T("5")],
                            &[T("35"), T("2020-01-04"), T("5")],
                            &[T("25"), T("2020-01-03"), T("5")],
                            &[T("15"), T("2020-01-02"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM sd WHERE g IS NULL AND v < 3 ORDER BY g, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, h, v FROM sd WHERE v = 3 ORDER BY h, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("h", TIMESTAMP), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("2020-01-01 01:00:00"), T("3")],
                            &[T("23"), T("2020-01-01 02:00:00"), T("3")],
                            &[T("33"), T("2020-01-01 03:00:00"), T("3")],
                            &[T("43"), T("2020-01-01 04:00:00"), T("3")],
                            &[T("53"), T("2020-01-01 05:00:00"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, h, v FROM sd WHERE v IN (2, 4) ORDER BY h DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("h", TIMESTAMP), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("2020-01-01 05:00:00"), T("4")],
                            &[T("52"), T("2020-01-01 05:00:00"), T("2")],
                            &[T("44"), T("2020-01-01 04:00:00"), T("4")],
                            &[T("42"), T("2020-01-01 04:00:00"), T("2")],
                            &[T("34"), T("2020-01-01 03:00:00"), T("4")],
                            &[T("32"), T("2020-01-01 03:00:00"), T("2")],
                            &[T("24"), T("2020-01-01 02:00:00"), T("4")],
                            &[T("22"), T("2020-01-01 02:00:00"), T("2")],
                            &[T("14"), T("2020-01-01 01:00:00"), T("4")],
                            &[T("12"), T("2020-01-01 01:00:00"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, h, v FROM sd WHERE v > 4 ORDER BY h DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("h", TIMESTAMP), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("2020-01-01 05:00:00"), T("5")],
                            &[T("45"), T("2020-01-01 04:00:00"), T("5")],
                            &[T("35"), T("2020-01-01 03:00:00"), T("5")],
                            &[T("25"), T("2020-01-01 02:00:00"), T("5")],
                            &[T("15"), T("2020-01-01 01:00:00"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, h, v FROM sd WHERE h IS NULL AND v < 3 ORDER BY h, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("h", TIMESTAMP), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, i, v FROM sd WHERE v = 3 ORDER BY i, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("i", TIMESTAMPTZ), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("2020-01-01 00:01:00+00"), T("3")],
                            &[T("23"), T("2020-01-01 00:02:00+00"), T("3")],
                            &[T("33"), T("2020-01-01 00:03:00+00"), T("3")],
                            &[T("43"), T("2020-01-01 00:04:00+00"), T("3")],
                            &[T("53"), T("2020-01-01 00:05:00+00"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, i, v FROM sd WHERE v IN (2, 4) ORDER BY i DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("i", TIMESTAMPTZ), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("2020-01-01 00:05:00+00"), T("4")],
                            &[T("52"), T("2020-01-01 00:05:00+00"), T("2")],
                            &[T("44"), T("2020-01-01 00:04:00+00"), T("4")],
                            &[T("42"), T("2020-01-01 00:04:00+00"), T("2")],
                            &[T("34"), T("2020-01-01 00:03:00+00"), T("4")],
                            &[T("32"), T("2020-01-01 00:03:00+00"), T("2")],
                            &[T("24"), T("2020-01-01 00:02:00+00"), T("4")],
                            &[T("22"), T("2020-01-01 00:02:00+00"), T("2")],
                            &[T("14"), T("2020-01-01 00:01:00+00"), T("4")],
                            &[T("12"), T("2020-01-01 00:01:00+00"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, i, v FROM sd WHERE v > 4 ORDER BY i DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("i", TIMESTAMPTZ), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("2020-01-01 00:05:00+00"), T("5")],
                            &[T("45"), T("2020-01-01 00:04:00+00"), T("5")],
                            &[T("35"), T("2020-01-01 00:03:00+00"), T("5")],
                            &[T("25"), T("2020-01-01 00:02:00+00"), T("5")],
                            &[T("15"), T("2020-01-01 00:01:00+00"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, i, v FROM sd WHERE i IS NULL AND v < 3 ORDER BY i, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("i", TIMESTAMPTZ), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, j, v FROM sd WHERE v = 3 ORDER BY j, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("j", UUID), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("00000000-0000-0000-0000-000000000001"), T("3")],
                            &[T("23"), T("00000000-0000-0000-0000-000000000002"), T("3")],
                            &[T("33"), T("00000000-0000-0000-0000-000000000003"), T("3")],
                            &[T("43"), T("00000000-0000-0000-0000-000000000004"), T("3")],
                            &[T("53"), T("00000000-0000-0000-0000-000000000005"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, j, v FROM sd WHERE v IN (2, 4) ORDER BY j DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("j", UUID), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("00000000-0000-0000-0000-000000000005"), T("4")],
                            &[T("52"), T("00000000-0000-0000-0000-000000000005"), T("2")],
                            &[T("44"), T("00000000-0000-0000-0000-000000000004"), T("4")],
                            &[T("42"), T("00000000-0000-0000-0000-000000000004"), T("2")],
                            &[T("34"), T("00000000-0000-0000-0000-000000000003"), T("4")],
                            &[T("32"), T("00000000-0000-0000-0000-000000000003"), T("2")],
                            &[T("24"), T("00000000-0000-0000-0000-000000000002"), T("4")],
                            &[T("22"), T("00000000-0000-0000-0000-000000000002"), T("2")],
                            &[T("14"), T("00000000-0000-0000-0000-000000000001"), T("4")],
                            &[T("12"), T("00000000-0000-0000-0000-000000000001"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, j, v FROM sd WHERE v > 4 ORDER BY j DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("j", UUID), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("00000000-0000-0000-0000-000000000005"), T("5")],
                            &[T("45"), T("00000000-0000-0000-0000-000000000004"), T("5")],
                            &[T("35"), T("00000000-0000-0000-0000-000000000003"), T("5")],
                            &[T("25"), T("00000000-0000-0000-0000-000000000002"), T("5")],
                            &[T("15"), T("00000000-0000-0000-0000-000000000001"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, j, v FROM sd WHERE j IS NULL AND v < 3 ORDER BY j, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("j", UUID), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM sd WHERE v = 3 ORDER BY k, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("k01"), T("3")],
                            &[T("23"), T("k02"), T("3")],
                            &[T("33"), T("k03"), T("3")],
                            &[T("43"), T("k04"), T("3")],
                            &[T("53"), T("k05"), T("3")],
                            &[T("63"), Null, T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM sd WHERE v IN (2, 4) ORDER BY k DESC NULLS FIRST, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("64"), Null, T("4")],
                            &[T("62"), Null, T("2")],
                            &[T("54"), T("k05"), T("4")],
                            &[T("52"), T("k05"), T("2")],
                            &[T("44"), T("k04"), T("4")],
                            &[T("42"), T("k04"), T("2")],
                            &[T("34"), T("k03"), T("4")],
                            &[T("32"), T("k03"), T("2")],
                            &[T("24"), T("k02"), T("4")],
                            &[T("22"), T("k02"), T("2")],
                            &[T("14"), T("k01"), T("4")],
                            &[T("12"), T("k01"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM sd WHERE v > 4 ORDER BY k DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("65"), Null, T("5")],
                            &[T("55"), T("k05"), T("5")],
                            &[T("45"), T("k04"), T("5")],
                            &[T("35"), T("k03"), T("5")],
                            &[T("25"), T("k02"), T("5")],
                            &[T("15"), T("k01"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM sd WHERE k IS NULL AND v < 3 ORDER BY k, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("61"), Null, T("1")],
                            &[T("62"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, e, v FROM sd WHERE v = 3 ORDER BY e, v, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("e", BOOL), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("f"), T("3")],
                            &[T("33"), T("f"), T("3")],
                            &[T("53"), T("f"), T("3")],
                            &[T("23"), T("t"), T("3")],
                            &[T("43"), T("t"), T("3")],
                            &[T("63"), T("t"), T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, e, v FROM sd WHERE v IN (2, 4) ORDER BY e DESC NULLS FIRST, v DESC, id DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("e", BOOL), Column("v", INT4)],
                        rows: &[
                            &[T("64"), T("t"), T("4")],
                            &[T("44"), T("t"), T("4")],
                            &[T("24"), T("t"), T("4")],
                            &[T("62"), T("t"), T("2")],
                            &[T("42"), T("t"), T("2")],
                            &[T("22"), T("t"), T("2")],
                            &[T("54"), T("f"), T("4")],
                            &[T("34"), T("f"), T("4")],
                            &[T("14"), T("f"), T("4")],
                            &[T("52"), T("f"), T("2")],
                            &[T("32"), T("f"), T("2")],
                            &[T("12"), T("f"), T("2")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, e, v FROM sd WHERE v > 4 ORDER BY e DESC, v DESC, id DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("e", BOOL), Column("v", INT4)],
                        rows: &[
                            &[T("65"), T("t"), T("5")],
                            &[T("45"), T("t"), T("5")],
                            &[T("25"), T("t"), T("5")],
                            &[T("55"), T("f"), T("5")],
                            &[T("35"), T("f"), T("5")],
                            &[T("15"), T("f"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, e, v FROM sd WHERE e IS NULL AND v < 3 ORDER BY e, v, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("e", BOOL), Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM sd WHERE b BETWEEN 2 AND 4 AND v = 5 ORDER BY b, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("25"), T("2"), T("5")],
                            &[T("35"), T("3"), T("5")],
                            &[T("45"), T("4"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM sd WHERE b > 2 AND v IN (1, 5) ORDER BY b DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("55"), T("5"), T("5")],
                            &[T("51"), T("5"), T("1")],
                            &[T("45"), T("4"), T("5")],
                            &[T("41"), T("4"), T("1")],
                            &[T("35"), T("3"), T("5")],
                            &[T("31"), T("3"), T("1")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, v FROM sd WHERE b < 3 AND v >= 4 ORDER BY b NULLS FIRST, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("14"), T("1"), T("4")],
                            &[T("15"), T("1"), T("5")],
                            &[T("24"), T("2"), T("4")],
                            &[T("25"), T("2"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, k, v FROM sd WHERE k >= 'k03' AND v = 2 ORDER BY k, v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("k", TEXT), Column("v", INT4)],
                        rows: &[
                            &[T("32"), T("k03"), T("2")],
                            &[T("42"), T("k04"), T("2")],
                            &[T("52"), T("k05"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, g, v FROM sd WHERE g > DATE '2020-01-03' AND v = 1 ORDER BY g DESC, v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", DATE), Column("v", INT4)],
                        rows: &[
                            &[T("51"), T("2020-01-06"), T("1")],
                            &[T("41"), T("2020-01-05"), T("1")],
                            &[T("31"), T("2020-01-04"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sd WHERE b IS NOT NULL AND v = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
    ]);
}

#[test]
fn test_btree_array_key_searches_cursors_and_merge_joins() {
    run_scripts(&[
        ScriptTest {
            name: "btree array keys' index searches, contradictions, scrollable cursors, merge joins, and bitmap scans",
            set_up_script: &[
                "CREATE TABLE small (id INT PRIMARY KEY, v INT);",
                "INSERT INTO small SELECT i, i % 4 FROM generate_series(1, 20) i;",
                "CREATE TABLE big (a INT, b INT, PRIMARY KEY (a, b));",
                "INSERT INTO big SELECT a, b FROM generate_series(1, 3) a, generate_series(1, 20000) b;",
                "CREATE TABLE x (k INT, n INT);",
                "INSERT INTO x VALUES (1, 1), (1, 2), (2, 3), (3, 4), (3, 5), (4, 6), (5, 7);",
                "CREATE TABLE y (k INT, m INT, PRIMARY KEY (k, m));",
                "INSERT INTO y SELECT k, m FROM generate_series(1, 5) k, generate_series(1, 2) m;",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
                "ANALYZE small;",
                "VACUUM ANALYZE big;",
                "ANALYZE x;",
                "ANALYZE y;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM small WHERE id IN (1, 2, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using small_pkey on small (actual rows=3.00 loops=1)")],
                            &[T("  Index Cond: (id = ANY ('{1,2,3}'::integer[]))")],
                            &[T("  Index Searches: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM small WHERE id IN (1, 5);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using small_pkey on small (actual rows=2.00 loops=1)")],
                            &[T("  Index Cond: (id = ANY ('{1,5}'::integer[]))")],
                            &[T("  Index Searches: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("Index Searches counts each re-seek at a gap between array keys' matches, not Postgres' page boundaries"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM big WHERE a IN (1, 3) AND b = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using big_pkey on big (actual rows=2.00 loops=1)")],
                            &[T("  Index Cond: ((a = ANY ('{1,3}'::integer[])) AND (b = 7))")],
                            &[T("  Heap Fetches: 0")],
                            &[T("  Index Searches: 2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM big WHERE b = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using big_pkey on big (actual rows=3.00 loops=1)")],
                            &[T("  Index Cond: (b = 7)")],
                            &[T("  Heap Fetches: 0")],
                            &[T("  Index Searches: 4")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("Index Searches counts each re-seek at a gap between array keys' matches, not Postgres' page boundaries"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM big WHERE b IN (7, 19000);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using big_pkey on big (actual rows=6.00 loops=1)")],
                            &[T("  Index Cond: (b = ANY ('{7,19000}'::integer[]))")],
                            &[T("  Heap Fetches: 0")],
                            &[T("  Index Searches: 7")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("Index Searches counts each re-seek at a gap between array keys' matches, not Postgres' page boundaries"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM big WHERE a = 2 AND b IN (1, 2, 3, 4);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using big_pkey on big (actual rows=4.00 loops=1)")],
                            &[T("  Index Cond: ((a = 2) AND (b = ANY ('{1,2,3,4}'::integer[])))")],
                            &[T("  Heap Fetches: 0")],
                            &[T("  Index Searches: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM small WHERE id IN (1, 2) AND id > 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using small_pkey on small (actual rows=0.00 loops=1)")],
                            &[T("  Index Cond: ((id = ANY ('{1,2}'::integer[])) AND (id > 5))")],
                            &[T("  Index Searches: 0")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM small WHERE id = ANY ('{}');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using small_pkey on small (actual rows=0.00 loops=1)")],
                            &[T("  Index Cond: (id = ANY ('{}'::integer[]))")],
                            &[T("  Index Searches: 0")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM small WHERE id IN (1, 2) AND id IN (3, 4);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM small WHERE id IN (1, 2, 3) AND id IN (3, 4) AND id < 4;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM small WHERE id = ANY ('{}');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM small WHERE id = ANY ('{NULL, 2}');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM small WHERE id IN (2, 4) AND id = ANY ('{4, 5}');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("4"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM big WHERE b = 20001;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM big WHERE a > 3 AND b = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT id FROM small WHERE id IN (3, 1, 7, 5) ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("5")],
                            &[T("7")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("7")],
                            &[T("5")],
                            &[T("3")],
                            &[T("1")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FORWARD 3 c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                            &[T("5")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE c;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE d SCROLL CURSOR FOR SELECT a, b FROM big WHERE b IN (5, 6) ORDER BY a DESC, b DESC;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("6")],
                            &[T("3"), T("5")],
                            &[T("2"), T("6")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("5")],
                            &[T("3"), T("6")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("5")],
                            &[T("2"), T("6")],
                            &[T("2"), T("5")],
                            &[T("1"), T("6")],
                            &[T("1"), T("5")],
                        ],
                        tag: "FETCH 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                            &[T("1"), T("6")],
                            &[T("2"), T("5")],
                            &[T("2"), T("6")],
                            &[T("3"), T("5")],
                            &[T("3"), T("6")],
                        ],
                        tag: "FETCH 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE d;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashjoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_nestloop = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_material = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT x.k, x.n, y.m FROM x JOIN y ON x.k = y.k WHERE y.k IN (1, 3, 4) ORDER BY x.k, x.n, y.m;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Incremental Sort")],
                            &[T("  Sort Key: x.k, x.n, y.m")],
                            &[T("  Presorted Key: x.k")],
                            &[T("  ->  Merge Join")],
                            &[T("        Merge Cond: (y.k = x.k)")],
                            &[T("        ->  Index Only Scan using y_pkey on y")],
                            &[T("              Index Cond: (k = ANY ('{1,3,4}'::integer[]))")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: x.k")],
                            &[T("              ->  Seq Scan on x")],
                            &[T("                    Disabled: true")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x.k, x.n, y.m FROM x JOIN y ON x.k = y.k WHERE y.k IN (1, 3, 4) ORDER BY x.k, x.n, y.m;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("n", INT4), Column("m", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("1"), T("1"), T("2")],
                            &[T("1"), T("2"), T("1")],
                            &[T("1"), T("2"), T("2")],
                            &[T("3"), T("4"), T("1")],
                            &[T("3"), T("4"), T("2")],
                            &[T("3"), T("5"), T("1")],
                            &[T("3"), T("5"), T("2")],
                            &[T("4"), T("6"), T("1")],
                            &[T("4"), T("6"), T("2")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x.k, x.n, y.m FROM x JOIN y ON x.k = y.k WHERE y.m = 2 ORDER BY x.k, x.n;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("n", INT4), Column("m", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("2")],
                            &[T("1"), T("2"), T("2")],
                            &[T("2"), T("3"), T("2")],
                            &[T("3"), T("4"), T("2")],
                            &[T("3"), T("5"), T("2")],
                            &[T("4"), T("6"), T("2")],
                            &[T("5"), T("7"), T("2")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_mergejoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashjoin = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM big WHERE a = 2 AND b IN (1, 2, 3, 4);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  ->  Bitmap Heap Scan on big (actual rows=4.00 loops=1)")],
                            &[T("        Recheck Cond: ((a = 2) AND (b = ANY ('{1,2,3,4}'::integer[])))")],
                            &[T("        Heap Blocks: exact=1")],
                            &[T("        ->  Bitmap Index Scan on big_pkey (actual rows=4.00 loops=1)")],
                            &[T("              Index Cond: ((a = 2) AND (b = ANY ('{1,2,3,4}'::integer[])))")],
                            &[T("              Index Searches: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("Heap Blocks counts leaf chunks, which depend on when the materializer last ran (Appendix C), so the exact count is not asserted; explain::test_explain_analyze_bitmap_heap_blocks covers the line"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM big WHERE b IN (7, 8) AND a < 3 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("7")],
                            &[T("1"), T("8")],
                            &[T("2"), T("7")],
                            &[T("2"), T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM big WHERE b = 19999 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("19999")],
                            &[T("2"), T("19999")],
                            &[T("3"), T("19999")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM small WHERE id IN (2, 4, 6) OR id = ANY ('{8, 20}') ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("4")],
                            &[T("6")],
                            &[T("8")],
                            &[T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM small WHERE id IN (2, 4) AND id > 3;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
fn test_btree_row_comparisons() {
    run_scripts(&[
        ScriptTest {
            name: "btree scans by row comparisons",
            set_up_script: &[
                "CREATE TABLE r (id INT PRIMARY KEY, a INT, b INT, c INT);",
                "INSERT INTO r SELECT a * 10 + b, a, b, b % 2 FROM generate_series(1, 4) a, generate_series(1, 4) b;",
                "INSERT INTO r VALUES (91, 2, NULL, 0), (92, NULL, 1, 1), (93, NULL, NULL, 0);",
                "CREATE INDEX r_ab ON r (a, b);",
                "CREATE TABLE rd (LIKE r);",
                "INSERT INTO rd SELECT * FROM r;",
                "CREATE INDEX rd_ab ON rd (a, b DESC);",
                "CREATE TABLE rn (LIKE r);",
                "INSERT INTO rn SELECT * FROM r;",
                "CREATE INDEX rn_ab ON rn (a NULLS FIRST, b NULLS FIRST);",
                "CREATE TABLE r3 (LIKE r);",
                "INSERT INTO r3 SELECT * FROM r;",
                "CREATE INDEX r3_abc ON r3 (a, b, c);",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
                "ANALYZE r;",
                "ANALYZE rd;",
                "ANALYZE rn;",
                "ANALYZE r3;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM r WHERE (a, b) > (2, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using r_ab on r")],
                            &[T("  Index Cond: (ROW(a, b) > ROW(2, 3))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan using r_ab on r", "  Index Cond: (ROW(a, b) > ROW(2, 3))"])),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) > (2, 3) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) >= (2, 3) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("23")],
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) < (2, 3) ORDER BY a DESC, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("22")],
                            &[T("21")],
                            &[T("14")],
                            &[T("13")],
                            &[T("12")],
                            &[T("11")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) <= (2, 3) ORDER BY a DESC, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("23")],
                            &[T("22")],
                            &[T("21")],
                            &[T("14")],
                            &[T("13")],
                            &[T("12")],
                            &[T("11")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) > (2, 3) ORDER BY a DESC, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("44")],
                            &[T("43")],
                            &[T("42")],
                            &[T("41")],
                            &[T("34")],
                            &[T("33")],
                            &[T("32")],
                            &[T("31")],
                            &[T("24")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) <= (2, 3) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("11")],
                            &[T("12")],
                            &[T("13")],
                            &[T("14")],
                            &[T("21")],
                            &[T("22")],
                            &[T("23")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) > (2, 3) AND (a, b) < (4, 2) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM r WHERE (a, c) > (2, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using r_ab on r")],
                            &[T("  Index Cond: (a >= 2)")],
                            &[T("  Filter: (ROW(a, c) > ROW(2, 0))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, c) > (2, 0) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("21")],
                            &[T("23")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM r WHERE (2, 3) < (a, b);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using r_ab on r")],
                            &[T("  Index Cond: (ROW(a, b) > ROW(2, 3))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan using r_ab on r", "  Index Cond: (ROW(a, b) > ROW(2, 3))"])),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (2, 3) < (a, b) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) >= (3, NULL) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) <= (2, NULL) ORDER BY a DESC, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("14")],
                            &[T("13")],
                            &[T("12")],
                            &[T("11")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) > (NULL, 1) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) > (1, 4) AND (a, b) < (3, 1) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("21")],
                            &[T("22")],
                            &[T("23")],
                            &[T("24")],
                            &[T("91")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM rd WHERE (a, b) > (2, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using rd_ab on rd")],
                            &[T("  Index Cond: (ROW(a, b) > ROW(2, 3))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rd WHERE (a, b) > (2, 3) ORDER BY a, b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("24")],
                            &[T("34")],
                            &[T("33")],
                            &[T("32")],
                            &[T("31")],
                            &[T("44")],
                            &[T("43")],
                            &[T("42")],
                            &[T("41")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rd WHERE (a, b) < (3, 2) ORDER BY a DESC, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("31")],
                            &[T("21")],
                            &[T("22")],
                            &[T("23")],
                            &[T("24")],
                            &[T("91")],
                            &[T("11")],
                            &[T("12")],
                            &[T("13")],
                            &[T("14")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rn WHERE (a, b) > (2, 3) ORDER BY a NULLS FIRST, b NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rn WHERE (a, b) < (2, 3) ORDER BY a DESC NULLS LAST, b DESC NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("22")],
                            &[T("21")],
                            &[T("14")],
                            &[T("13")],
                            &[T("12")],
                            &[T("11")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rn WHERE (a, b) >= (2, 1) AND (a, b) <= (2, 4) ORDER BY a NULLS FIRST, b NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("21")],
                            &[T("22")],
                            &[T("23")],
                            &[T("24")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM r3 WHERE (a, b, c) > (2, 3, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using r3_abc on r3")],
                            &[T("  Index Cond: (ROW(a, b, c) > ROW(2, 3, 0))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r3 WHERE (a, b, c) > (2, 3, 0) ORDER BY a, b, c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("23")],
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r3 WHERE (a, b, c) <= (3, 2, 0) ORDER BY a DESC, b DESC, c DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("32")],
                            &[T("31")],
                            &[T("91")],
                            &[T("24")],
                            &[T("23")],
                            &[T("22")],
                            &[T("21")],
                            &[T("14")],
                            &[T("13")],
                            &[T("12")],
                            &[T("11")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r3 WHERE (a, c) >= (2, 1) ORDER BY a, b, c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("21")],
                            &[T("23")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) > (2::INT8, 3::INT2) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) > (2, 3.5) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE a IN (1, 3) AND (a, b) > (1, 2) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("13")],
                            &[T("14")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) > (1, 2) AND b = 4 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("14")],
                            &[T("24")],
                            &[T("34")],
                            &[T("44")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM r WHERE (a, b) > (3, 2) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("3"), T("4")],
                            &[T("4"), T("1")],
                            &[T("4"), T("2")],
                            &[T("4"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plan_cache_mode = force_generic_plan;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE p (INT, INT) AS SELECT id FROM r WHERE (a, b) > ($1, $2) ORDER BY a, b;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE p (2, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using r_ab on r")],
                            &[T("  Index Cond: (ROW(a, b) > ROW($1, $2))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    diverged: Some((PRIMARY_KEY_RETURNED, &["Index Only Scan using r_ab on r", "  Index Cond: (ROW(a, b) > ROW($1, $2))"])),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p (2, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("24")],
                            &[T("31")],
                            &[T("32")],
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p (3, NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE p (NULL, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DEALLOCATE p;",
                    expected: Expected::Tag("DEALLOCATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET plan_cache_mode;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM r WHERE (a, b) >= (3, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on r")],
                            &[T("  Recheck Cond: (ROW(a, b) >= ROW(3, 3))")],
                            &[T("  ->  Bitmap Index Scan on r_ab")],
                            &[T("        Index Cond: (ROW(a, b) >= ROW(3, 3))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM r WHERE (a, b) >= (3, 3) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("33")],
                            &[T("34")],
                            &[T("41")],
                            &[T("42")],
                            &[T("43")],
                            &[T("44")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_bitmap_index_scans_whose_entries_bytes_decide_their_keys() {
    run_scripts(&[
        ScriptTest {
            name: "bitmap index scans whose keys their entries' key bytes decide",
            set_up_script: &[
                "SET enable_seqscan = off;",
                "SET enable_indexscan = off;",
                "SET enable_indexonlyscan = off;",
                "CREATE TABLE t (id INTEGER PRIMARY KEY, k INTEGER, d INTEGER, s TEXT COLLATE \"en-x-icu\", b BIGINT);",
                "INSERT INTO t SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 500 END, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 300 END, 'v' || (i % 400), i FROM generate_series(1, 3000) i;",
                "CREATE INDEX t_k ON t (k);",
                "CREATE INDEX t_d ON t (d DESC NULLS LAST);",
                "CREATE INDEX t_kn ON t (k NULLS FIRST);",
                "CREATE INDEX t_s ON t (s);",
                "CREATE INDEX t_kd ON t (k, d);",
                "ANALYZE t;",
                "CREATE TABLE u (id INTEGER PRIMARY KEY, k INTEGER UNIQUE);",
                "INSERT INTO u SELECT i, i * 3 FROM generate_series(1, 2000) i;",
                "ANALYZE u;",
                "CREATE TABLE kl (k INTEGER, v TEXT);",
                "INSERT INTO kl SELECT i % 50, 'x' FROM generate_series(1, 2000) i;",
                "CREATE INDEX kl_k ON kl (k);",
                "ANALYZE kl;",
                "CREATE TABLE dp (id INTEGER PRIMARY KEY DEFERRABLE, k INTEGER);",
                "INSERT INTO dp SELECT i, i % 100 FROM generate_series(1, 2000) i;",
                "CREATE INDEX dp_k ON dp (k);",
                "ANALYZE dp;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("58"), T("75261")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k <= 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("63"), T("82311")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k = 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("5"), T("7050")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k >= 490;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("56"), T("97190")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k > 490;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("50"), T("86750")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k BETWEEN 100 AND 120 OR k BETWEEN 300 AND 301;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("130"), T("179086")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE d < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("94"), T("126711")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE d >= 290;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("90"), T("148005")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE d BETWEEN 50 AND 60;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("102"), T("142707")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("176"), T("264792")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k IS NOT NULL AND k > 495;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("22"), T("37945")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k > 2147483647::BIGINT;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("0"), Null]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k >= 400::BIGINT;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("564"), T("958500")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k < 5000000000::BIGINT AND k > 480;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("107"), T("185423")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k >= -5000000000::BIGINT AND k < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("18"), T("25518")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE s >= 'v1' AND s < 'v2';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("888"), T("1363968")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE s BETWEEN 'V10' AND 'v11';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("88"), T("131648")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k = 7 AND d > 100;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("3"), T("5521")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k BETWEEN 7 AND 9 AND d < 50;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("5"), T("3040")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k = ANY (ARRAY[1, 5, 499]);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("17"), T("23031")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM u WHERE k BETWEEN 300 AND 900;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("201"), T("40200")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM kl WHERE k BETWEEN 10 AND 20;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[&[T("440")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM dp WHERE k BETWEEN 10 AND 20;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("220"), T("212300")]],
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
                    query: "UPDATE t SET k = 105 WHERE id IN (1, 2, 3);",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE id IN (4, 5) ;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (5000, 110, 1, 'z', 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(id) FROM t WHERE k BETWEEN 100 AND 120;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[&[T("122"), T("165486")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
