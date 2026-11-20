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
use harness::script::{A, Column, Diagnostic, E, Expected, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};


#[test]
fn test_range_keys() {
    run_scripts(&[
        ScriptTest {
            name: "int4range primary keys order, look up, and refuse duplicates as range_cmp compares them",
            set_up_script: &[
                "CREATE TABLE r (k INT4RANGE PRIMARY KEY, v INT);",
                "INSERT INTO r VALUES ('[1,3)', 1), ('empty', 2), ('(,5)', 3), ('[1,)', 4), ('(,)', 5), ('[-5,0)', 6), ('[1,1]', 7), ('[0,3)', 8), ('(,1)', 9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT k, v FROM r ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4RANGE), Column("v", INT4)],
                        rows: &[
                            &[T("empty"), T("2")],
                            &[T("(,1)"), T("9")],
                            &[T("(,5)"), T("3")],
                            &[T("(,)"), T("5")],
                            &[T("[-5,0)"), T("6")],
                            &[T("[0,3)"), T("8")],
                            &[T("[1,2)"), T("7")],
                            &[T("[1,3)"), T("1")],
                            &[T("[1,)"), T("4")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM r ORDER BY k DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4RANGE), Column("v", INT4)],
                        rows: &[
                            &[T("[1,)"), T("4")],
                            &[T("[1,3)"), T("1")],
                            &[T("[1,2)"), T("7")],
                            &[T("[0,3)"), T("8")],
                            &[T("[-5,0)"), T("6")],
                            &[T("(,)"), T("5")],
                            &[T("(,5)"), T("3")],
                            &[T("(,1)"), T("9")],
                            &[T("empty"), T("2")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM r WHERE k = '(0,2]';",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM r WHERE k = 'empty';",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r VALUES ('(0,2]', 10);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"r_pkey\"", detail: "Key (k)=([1,3)) already exists.", schema: "public", table: "r", constraint: "r_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r VALUES ('[3,3)', 11);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"r_pkey\"", detail: "Key (k)=(empty) already exists.", schema: "public", table: "r", constraint: "r_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM r WHERE k > '[1,2)' ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4RANGE), Column("v", INT4)],
                        rows: &[
                            &[T("[1,3)"), T("1")],
                            &[T("[1,)"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM r WHERE k <= '[0,3)' ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4RANGE), Column("v", INT4)],
                        rows: &[
                            &[T("empty"), T("2")],
                            &[T("(,1)"), T("9")],
                            &[T("(,5)"), T("3")],
                            &[T("(,)"), T("5")],
                            &[T("[-5,0)"), T("6")],
                            &[T("[0,3)"), T("8")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r SET k = '[10,20)' WHERE v = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM r WHERE k @> 15 ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4RANGE), Column("v", INT4)],
                        rows: &[
                            &[T("(,)"), T("5")],
                            &[T("[1,)"), T("4")],
                            &[T("[10,20)"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r WHERE k = '[10,20)';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM r;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "numrange primary keys compare numerics by value and keep each value's display scale",
            set_up_script: &[
                "CREATE TABLE nr (k NUMRANGE PRIMARY KEY, v INT);",
                "INSERT INTO nr VALUES ('[1.0,2)', 1), ('[1,2]', 2), ('(1,2)', 3), ('[1.50,)', 4), ('empty', 5), ('(,0.000)', 6);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT k, v FROM nr ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", NUMRANGE), Column("v", INT4)],
                        rows: &[
                            &[T("empty"), T("5")],
                            &[T("(,0.000)"), T("6")],
                            &[T("[1.0,2)"), T("1")],
                            &[T("[1,2]"), T("2")],
                            &[T("(1,2)"), T("3")],
                            &[T("[1.50,)"), T("4")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO nr VALUES ('[1,2.00)', 7);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"nr_pkey\"", detail: "Key (k)=([1,2.00)) already exists.", schema: "public", table: "nr", constraint: "nr_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM nr WHERE k = '[1.00,2)';",
                    expected: Expected::Rows {
                        columns: &[Column("k", NUMRANGE), Column("v", INT4)],
                        rows: &[
                            &[T("[1.0,2)"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM nr WHERE k = '[1.5,)';",
                    expected: Expected::Rows {
                        columns: &[Column("k", NUMRANGE), Column("v", INT4)],
                        rows: &[
                            &[T("[1.50,)"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "text range keys whose lower bounds are prefixes of others order as their collations compare",
            set_up_script: &[
                "CREATE TYPE textrange_c AS RANGE (subtype = text, collation = \"C\");",
                "CREATE TYPE textrange_icu AS RANGE (subtype = text, collation = \"en-x-icu\");",
                "CREATE TABLE tc (k textrange_c PRIMARY KEY);",
                "CREATE TABLE ti (k textrange_icu PRIMARY KEY);",
                "INSERT INTO tc VALUES ('[a,z)'), ('[ab,z)'), ('[a,b)'), ('[a,)'), ('(,a)'), ('empty'), ('(a,z)'), ('[B,c)'), ('[a,ab]'), ('[\"\",a)');",
                "INSERT INTO ti VALUES ('[a,z)'), ('[ab,z)'), ('[a,b)'), ('[a,)'), ('(,a)'), ('empty'), ('(a,z)'), ('[B,c)'), ('[a,ab]'), ('[\"\",a)');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT k FROM tc ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", USER_DEFINED)],
                        rows: &[
                            &[T("empty")],
                            &[T("(,a)")],
                            &[T("[\"\",a)")],
                            &[T("[B,c)")],
                            &[T("[a,ab]")],
                            &[T("[a,b)")],
                            &[T("[a,z)")],
                            &[T("[a,)")],
                            &[T("(a,z)")],
                            &[T("[ab,z)")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM ti ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", USER_DEFINED)],
                        rows: &[
                            &[T("empty")],
                            &[T("(,a)")],
                            &[T("[\"\",a)")],
                            &[T("[a,ab]")],
                            &[T("[a,b)")],
                            &[T("[a,z)")],
                            &[T("[a,)")],
                            &[T("(a,z)")],
                            &[T("[ab,z)")],
                            &[T("[B,c)")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM tc WHERE k = '[ab,z)';",
                    expected: Expected::Rows {
                        columns: &[Column("k", USER_DEFINED)],
                        rows: &[
                            &[T("[ab,z)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM ti WHERE k < '[a,z)' ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", USER_DEFINED)],
                        rows: &[
                            &[T("empty")],
                            &[T("(,a)")],
                            &[T("[\"\",a)")],
                            &[T("[a,ab]")],
                            &[T("[a,b)")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tc VALUES ('[ab,z)');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"tc_pkey\"", detail: "Key (k)=([ab,z)) already exists.", schema: "public", table: "tc", constraint: "tc_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lower(k), upper(k) FROM ti WHERE k > '[a,b)' ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("lower", TEXT), Column("upper", TEXT)],
                        rows: &[
                            &[T("a"), T("z")],
                            &[T("a"), Null],
                            &[T("a"), T("z")],
                            &[T("ab"), T("z")],
                            &[T("B"), T("c")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "btree indexes on range columns, ascending and descending, with NULLs and empty ranges",
            set_up_script: &[
                "CREATE TABLE s (id INT PRIMARY KEY, r INT4RANGE, n NUMRANGE);",
                "INSERT INTO s SELECT i, int4range(i % 7, i % 7 + i % 3 + 1), numrange((i % 5)::numeric / 2, NULL) FROM generate_series(1, 40) i;",
                "INSERT INTO s VALUES (41, 'empty', 'empty'), (42, NULL, NULL), (43, '(,)', '[1.50,)');",
                "CREATE INDEX s_r ON s (r);",
                "CREATE INDEX s_n ON s (n DESC);",
                "CREATE UNIQUE INDEX s_rn ON s (r, id);",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, r FROM s WHERE r = '[3,5)' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("r", INT4RANGE)],
                        rows: &[
                            &[T("10"), T("[3,5)")],
                            &[T("31"), T("[3,5)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r, count(*) FROM s WHERE r < '[1,2)' GROUP BY r ORDER BY r;",
                    expected: Expected::Rows {
                        columns: &[Column("r", INT4RANGE), Column("count", INT8)],
                        rows: &[
                            &[T("empty"), T("1")],
                            &[T("(,)"), T("1")],
                            &[T("[0,1)"), T("1")],
                            &[T("[0,2)"), T("2")],
                            &[T("[0,3)"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, r FROM s WHERE r >= '[6,8)' ORDER BY r, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("r", INT4RANGE)],
                        rows: &[
                            &[T("13"), T("[6,8)")],
                            &[T("34"), T("[6,8)")],
                            &[T("20"), T("[6,9)")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, n FROM s WHERE n = '[1.5,)' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("n", NUMRANGE)],
                        rows: &[
                            &[T("3"), T("[1.5000000000000000,)")],
                            &[T("8"), T("[1.5000000000000000,)")],
                            &[T("13"), T("[1.5000000000000000,)")],
                            &[T("18"), T("[1.5000000000000000,)")],
                            &[T("23"), T("[1.5000000000000000,)")],
                            &[T("28"), T("[1.5000000000000000,)")],
                            &[T("33"), T("[1.5000000000000000,)")],
                            &[T("38"), T("[1.5000000000000000,)")],
                            &[T("43"), T("[1.50,)")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT n FROM s ORDER BY n DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("n", NUMRANGE)],
                        rows: &[
                            &[Null],
                            &[T("[2.0000000000000000,)")],
                            &[T("[1.5000000000000000,)")],
                            &[T("[1.00000000000000000000,)")],
                            &[T("[0.50000000000000000000,)")],
                            &[T("[0.00000000000000000000,)")],
                            &[T("empty")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM s WHERE r IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, r FROM s ORDER BY r NULLS FIRST, id LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("r", INT4RANGE)],
                        rows: &[
                            &[T("42"), Null],
                            &[T("41"), T("empty")],
                            &[T("43"), T("(,)")],
                            &[T("21"), T("[0,1)")],
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

#[test]
fn test_create_type_as_range() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TYPE AS RANGE takes its options and refuses bad ones as Postgres does",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TYPE r1 AS RANGE (subtype = int4, subtype_diff = int4range_subdiff);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rngtypid::regtype, rngsubtype::regtype, rngmultitypid::regtype, rngsubopc, rngcanonical, rngsubdiff FROM pg_range WHERE rngtypid = 'r1'::regtype;",
                    expected: Expected::Rows {
                        columns: &[Column("rngtypid", REGTYPE), Column("rngsubtype", REGTYPE), Column("rngmultitypid", REGTYPE), Column("rngsubopc", OID), Column("rngcanonical", REGPROC), Column("rngsubdiff", REGPROC)],
                        rows: &[
                            &[T("r1"), T("integer"), T("r1_multirange"), T("1978"), T("-"), T("int4range_subdiff")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r1(1, 5), r1(1, 5, '(]'), r1(NULL, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("r1", USER_DEFINED), Column("r1", USER_DEFINED), Column("r1", USER_DEFINED)],
                        rows: &[
                            &[T("[1,5)"), T("(1,5]"), T("(,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r2 AS RANGE (subtype = float8, multirange_type_name = r2_multi);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rngmultitypid::regtype FROM pg_range WHERE rngtypid = 'r2'::regtype;",
                    expected: Expected::Rows {
                        columns: &[Column("rngmultitypid", REGTYPE)],
                        rows: &[
                            &[T("r2_multi")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r3 AS RANGE (subtype = text, subtype_opclass = text_pattern_ops);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r3('a', 'b') < r3('a', 'c');",
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
                    query: "CREATE TYPE r4 AS RANGE (subtype = int4, collation = \"C\");",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "range collation specified but subtype does not support collation", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r5 AS RANGE (foo = int4);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "type attribute \"foo\" not recognized", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r6 AS RANGE (subtype_diff = int4range_subdiff);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "type attribute \"subtype\" is required", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r7 AS RANGE (subtype = anyelement);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "range subtype cannot be anyelement", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r8 AS RANGE (subtype = int4, canonical = int4range_canonical);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "cannot specify a canonical function without a pre-created shell type", hint: "Create the type as a shell type, then create its canonicalization function, then do a full CREATE TYPE.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r9 AS RANGE (subtype = int4, subtype_diff = float8mi);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function float8mi(integer, integer) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r10 AS RANGE (subtype = int4, subtype_opclass = text_ops);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "operator class \"text_ops\" does not accept data type integer", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r11 AS RANGE (subtype = int4, multirange_type_name = r1);",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "type \"r1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r1 AS RANGE (subtype = int8);",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "type \"r1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r12 AS RANGE (subtype = int4, subtype = int8);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "conflicting or redundant options", position: 43, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE r13 AS RANGE (subtype = int4, subtype_diff = nonexistent_fn);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nonexistent_fn(integer, integer) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tr (k r2 PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES ('[1.5,2)'), ('[-0,1)'), ('[NaN,NaN]'), ('[-Infinity,0]'), ('[2,NaN)');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES ('[0,1)');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"tr_pkey\"", detail: "Key (k)=([0,1)) already exists.", schema: "public", table: "tr", constraint: "tr_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM tr ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", USER_DEFINED)],
                        rows: &[
                            &[T("[-Infinity,0]")],
                            &[T("[-0,1)")],
                            &[T("[1.5,2)")],
                            &[T("[2,NaN)")],
                            &[T("[NaN,NaN]")],
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
fn test_range_containment_simplification() {
    run_scripts(&[
        ScriptTest {
            name: "a test of whether a constant range holds an element becomes comparisons with the range's bounds",
            set_up_script: &[
                "CREATE TYPE textrange_c AS RANGE (subtype = text, collation = \"C\");",
                "CREATE TABLE e (a INT, d DATE, t TEXT COLLATE \"C\");",
                "INSERT INTO e VALUES (1, '2000-01-01', 'b'), (5, '2000-06-01', 'm'), (10, NULL, 'z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM e WHERE a <@ int4range(1, 10);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on e")],
                            &[T("  Filter: ((a >= 1) AND (a < 10))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM e WHERE int4range(1, 10, '[]') @> a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on e")],
                            &[T("  Filter: ((a >= 1) AND (a < 11))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM e WHERE a <@ int4range(NULL, 5);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on e")],
                            &[T("  Filter: (a < 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM e WHERE a <@ 'empty'::int4range;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Result")],
                            &[T("  One-Time Filter: false")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM e WHERE d <@ daterange(NULL, NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on e")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM e WHERE (a + random()::int) <@ int4range(1, 10);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on e")],
                            &[T("  Filter: ((a + (random())::integer) <@ '[1,10)'::int4range)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM e WHERE (a + random()::int) <@ int4range(1, NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on e")],
                            &[T("  Filter: ((a + (random())::integer) >= 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM e WHERE t <@ textrange_c('a', 'n');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on e")],
                            &[T("  Filter: ((t >= 'a'::text) AND (t < 'n'::text))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM e WHERE a <@ int4range(1, 10) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM e WHERE int4range(1, 10, '[]') @> a ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("5")],
                            &[T("10")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM e WHERE d <@ daterange('2000-03-01', NULL) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM e WHERE t <@ textrange_c('a', 'n') ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("5")],
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
fn test_range_hashes() {
    run_scripts(&[
        ScriptTest {
            name: "range hashes and extended hashes match Postgres'",
            set_up_script: &[
                "CREATE TYPE textrange_c AS RANGE (subtype = text, collation = \"C\");",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT hash_range('[1,3)'::int4range), hash_range('empty'::int4range), hash_range('(,)'::int4range), hash_range('[a,b]'::textrange_c);",
                    expected: Expected::Rows {
                        columns: &[Column("hash_range", INT4), Column("hash_range", INT4), Column("hash_range", INT4), Column("hash_range", INT4)],
                        rows: &[
                            &[T("1703467254"), T("484847245"), T("1061745599"), T("951769890")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hash_range_extended('[1,3)'::int4range, 42), hash_range_extended('empty'::int4range, 42), hash_range_extended('(,3)'::int4range, 0), hash_range_extended('[1.5,)'::numrange, 7);",
                    expected: Expected::Rows {
                        columns: &[Column("hash_range_extended", INT8), Column("hash_range_extended", INT8), Column("hash_range_extended", INT8), Column("hash_range_extended", INT8)],
                        rows: &[
                            &[T("-33040569261725878"), T("4415371863373266702"), T("721923232793066398"), T("-1668132511968712126")],
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
fn test_range_keys_by_the_subtype_class() {
    run_scripts(&[
        ScriptTest {
            name: "range keys follow the range type's subtype operator class, a reversed one and a pattern one",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(a INT, b INT) RETURNS INT IMMUTABLE LANGUAGE sql AS $$ SELECT btint4cmp(b, a) $$;",
                "CREATE OPERATOR CLASS int4_rev_ops FOR TYPE INT USING btree AS OPERATOR 1 >, OPERATOR 2 >=, OPERATOR 3 =, OPERATOR 4 <=, OPERATOR 5 <, FUNCTION 1 rev_cmp(INT, INT);",
                "CREATE TYPE revrange AS RANGE (subtype = int4, subtype_opclass = int4_rev_ops);",
                "CREATE TYPE patrange AS RANGE (subtype = text, subtype_opclass = text_pattern_ops, collation = \"en-x-icu\");",
                "CREATE TABLE rv (id INT PRIMARY KEY, r revrange);",
                "INSERT INTO rv VALUES (1, revrange(5, 1)), (2, revrange(9, 7)), (3, revrange(5, 3)), (4, revrange(5, 5)), (5, revrange(20, NULL)), (6, revrange(NULL, 0, '(]'));",
                "CREATE TABLE pt (k patrange PRIMARY KEY);",
                "INSERT INTO pt VALUES ('[a,z)'), ('[B,c)'), ('[ab,b)'), ('[A,a)'), ('empty');",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, r FROM rv ORDER BY r;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("r", USER_DEFINED)],
                        rows: &[
                            &[T("4"), T("empty")],
                            &[T("6"), T("(,0]")],
                            &[T("5"), T("[20,)")],
                            &[T("2"), T("[9,7)")],
                            &[T("3"), T("[5,3)")],
                            &[T("1"), T("[5,1)")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX rv_r ON rv (r);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX rv_ru ON rv (r);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, r FROM rv ORDER BY r DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("r", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[5,1)")],
                            &[T("3"), T("[5,3)")],
                            &[T("2"), T("[9,7)")],
                            &[T("5"), T("[20,)")],
                            &[T("6"), T("(,0]")],
                            &[T("4"), T("empty")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM rv WHERE r = revrange(9, 7);",
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
                    query: "INSERT INTO rv VALUES (7, revrange(9, 7));",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"rv_ru\"", detail: "Key (r)=([9,7)) already exists.", schema: "public", table: "rv", constraint: "rv_ru", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM pt ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", USER_DEFINED)],
                        rows: &[
                            &[T("empty")],
                            &[T("[A,a)")],
                            &[T("[B,c)")],
                            &[T("[a,z)")],
                            &[T("[ab,b)")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM pt WHERE k = '[ab,b)';",
                    expected: Expected::Rows {
                        columns: &[Column("k", USER_DEFINED)],
                        rows: &[
                            &[T("[ab,b)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES ('[ab,b)');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"pt_pkey\"", detail: "Key (k)=([ab,b)) already exists.", schema: "public", table: "pt", constraint: "pt_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[a,z)'::patrange < '[B,c)'::patrange, revrange(5, 1) < revrange(9, 7);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[5,1)'::revrange;",
                    expected: Expected::Rows {
                        columns: &[Column("revrange", USER_DEFINED)],
                        rows: &[
                            &[T("[5,1)")],
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
fn test_multirange_keys() {
    run_scripts(&[
        ScriptTest {
            name: "multirange primary keys and indexes order, look up, and refuse duplicates as multirange_cmp compares them",
            set_up_script: &[
                "CREATE TABLE m (k INT4MULTIRANGE PRIMARY KEY, v INT);",
                "INSERT INTO m VALUES ('{}', 1), ('{[1,2)}', 2), ('{[1,2),[3,4)}', 3), ('{[1,3)}', 4), ('{(,1)}', 5), ('{(,1),[5,)}', 6), ('{(,)}', 7), ('{[0,1)}', 8);",
                "CREATE TABLE nm (id INT PRIMARY KEY, n NUMMULTIRANGE);",
                "INSERT INTO nm VALUES (1, '{[1.0,2)}'), (2, '{[1,2),[3.5,4]}'), (3, NULL), (4, '{}'), (5, '{[1.50,)}'), (6, '{[1.00,2)}');",
                "CREATE INDEX nm_n ON nm (n);",
                "SET enable_seqscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT k, v FROM m ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4MULTIRANGE), Column("v", INT4)],
                        rows: &[
                            &[T("{}"), T("1")],
                            &[T("{(,1)}"), T("5")],
                            &[T("{(,1),[5,)}"), T("6")],
                            &[T("{(,)}"), T("7")],
                            &[T("{[0,1)}"), T("8")],
                            &[T("{[1,2)}"), T("2")],
                            &[T("{[1,2),[3,4)}"), T("3")],
                            &[T("{[1,3)}"), T("4")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM m ORDER BY k DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4MULTIRANGE), Column("v", INT4)],
                        rows: &[
                            &[T("{[1,3)}"), T("4")],
                            &[T("{[1,2),[3,4)}"), T("3")],
                            &[T("{[1,2)}"), T("2")],
                            &[T("{[0,1)}"), T("8")],
                            &[T("{(,)}"), T("7")],
                            &[T("{(,1),[5,)}"), T("6")],
                            &[T("{(,1)}"), T("5")],
                            &[T("{}"), T("1")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM m WHERE k = '{[1,2), [3,4)}';",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO m VALUES ('{[1,2), [2,3)}', 9);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"m_pkey\"", detail: "Key (k)=({[1,3)}) already exists.", schema: "public", table: "m", constraint: "m_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO m VALUES ('{[3,4), [1,2)}', 10);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"m_pkey\"", detail: "Key (k)=({[1,2),[3,4)}) already exists.", schema: "public", table: "m", constraint: "m_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, n FROM nm WHERE n = '{[1,2)}' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("n", NUMMULTIRANGE)],
                        rows: &[
                            &[T("1"), T("{[1.0,2)}")],
                            &[T("6"), T("{[1.00,2)}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT n FROM nm ORDER BY n NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("n", NUMMULTIRANGE)],
                        rows: &[
                            &[Null],
                            &[T("{}")],
                            &[T("{[1.0,2)}")],
                            &[T("{[1,2),[3.5,4]}")],
                            &[T("{[1.50,)}")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, count(*) FROM nm GROUP BY n ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", NUMMULTIRANGE), Column("count", INT8)],
                        rows: &[
                            &[T("{}"), T("1")],
                            &[T("{[1.0,2)}"), T("2")],
                            &[T("{[1,2),[3.5,4]}"), T("1")],
                            &[T("{[1.50,)}"), T("1")],
                            &[Null, T("1")],
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
fn test_multirange_functions() {
    run_scripts(&[
        ScriptTest {
            name: "multirange constructors, operators, aggregates, hashes, input errors, and a multirange taking an array type's name",
            set_up_script: &[
                "CREATE TABLE ra (g INT, r INT4RANGE, m INT4MULTIRANGE);",
                "INSERT INTO ra VALUES (1, '[1,3)', '{[1,2)}'), (1, '[2,5)', '{[4,6)}'), (1, NULL, NULL), (2, '[10,20)', '{}'), (2, 'empty', '{[15,16)}'), (3, NULL, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT g, range_agg(r), range_agg(m), range_intersect_agg(r), range_intersect_agg(m) FROM ra GROUP BY g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("range_agg", INT4MULTIRANGE), Column("range_agg", INT4MULTIRANGE), Column("range_intersect_agg", INT4RANGE), Column("range_intersect_agg", INT4MULTIRANGE)],
                        rows: &[
                            &[T("1"), T("{[1,5)}"), T("{[1,2),[4,6)}"), T("[2,3)"), T("{}")],
                            &[T("2"), T("{[10,20)}"), T("{[15,16)}"), T("empty"), T("{}")],
                            &[T("3"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT range_agg(r) FROM ra WHERE false;",
                    expected: Expected::Rows {
                        columns: &[Column("range_agg", INT4MULTIRANGE)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, unnest(range_agg(r)) FROM ra GROUP BY g ORDER BY g, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("unnest", INT4RANGE)],
                        rows: &[
                            &[T("1"), T("[1,5)")],
                            &[T("2"), T("[10,20)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT int4multirange(), int4multirange(int4range(5, 7), int4range(1, 3), int4range(2, 4)), multirange(int4range(1, 4)), int4range(1, 4)::int4multirange;",
                    expected: Expected::Rows {
                        columns: &[Column("int4multirange", INT4MULTIRANGE), Column("int4multirange", INT4MULTIRANGE), Column("multirange", INT4MULTIRANGE), Column("int4range", INT4MULTIRANGE)],
                        rows: &[
                            &[T("{}"), T("{[1,4),[5,7)}"), T("{[1,4)}"), T("{[1,4)}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT int4multirange(VARIADIC ARRAY[int4range(1, 2), NULL]);",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "multirange values cannot contain null members", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT int4multirange(VARIADIC ARRAY[[int4range(1, 2)]]);",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "multiranges cannot be constructed from multidimensional arrays", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{[1,5), [8,9)}'::int4multirange - '{[2,3), [8,10)}', '{[1,5)}'::int4multirange * '{[2,8)}', '{[1,2)}'::int4multirange + '{[2,3)}';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4MULTIRANGE), Column("?column?", INT4MULTIRANGE), Column("?column?", INT4MULTIRANGE)],
                        rows: &[
                            &[T("{[1,2),[3,5)}"), T("{[2,5)}"), T("{[1,3)}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT range_merge('{[1,2), [5,8)}'::int4multirange), range_merge('{}'::int4multirange), lower('{[1,2), [5,8)}'::int4multirange), upper('{[1,2), (,)}'::int4multirange), isempty('{}'::int4multirange);",
                    expected: Expected::Rows {
                        columns: &[Column("range_merge", INT4RANGE), Column("range_merge", INT4RANGE), Column("lower", INT4), Column("upper", INT4), Column("isempty", BOOL)],
                        rows: &[
                            &[T("[1,8)"), T("empty"), T("1"), Null, T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{[1,3), [5,7)}'::int4multirange @> 6, 4 <@ '{[1,3), [5,7)}'::int4multirange, '{[1,3), [5,7)}'::int4multirange @> '{[5,6)}'::int4multirange, '{[1,3), [5,7)}'::int4multirange && int4range(3, 5);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{[1,3), [5,7)}'::int4multirange << int4range(8, 9), '{[1,3)}'::int4multirange &< '{[2,4)}', '{[5,7)}'::int4multirange &> int4range(4, 6), '{[1,3)}'::int4multirange -|- '{[3,5)}';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{[1,3), [5,7)}'::int4multirange < '{[1,3), [5,8)}', '{[1,3)}'::int4multirange < '{[1,3), [5,8)}', '{}'::int4multirange < '{[1,3)}', '{[1,3)}'::int4multirange = '{[1,2), [2,3)}';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hash_multirange('{[1,3), [5,7)}'::int4multirange), hash_multirange_extended('{[1,3), [5,7)}'::int4multirange, 42), hash_multirange('{}'::int4multirange);",
                    expected: Expected::Rows {
                        columns: &[Column("hash_multirange", INT4), Column("hash_multirange_extended", INT8), Column("hash_multirange", INT4)],
                        rows: &[
                            &[T("1871790709"), T("7050646914535429779"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{[1,3), [5,7)'::int4multirange;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "malformed multirange literal: \"{[1,3), [5,7)\"", position: 8, detail: "Unexpected end of input.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ' { [1,3) , empty , \"[5,7)\" } '::int4multirange;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "malformed multirange literal: \" { [1,3) , empty , \"[5,7)\" } \"", position: 8, detail: "Expected range start.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE textrange1 AS RANGE (subtype = text, multirange_type_name = multirange_of_text, collation = \"C\");",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE textrange2 AS RANGE (subtype = text, multirange_type_name = _textrange1, collation = \"C\");",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typname, typtype FROM pg_type WHERE typname LIKE '%textrange%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("typtype", CHAR)],
                        rows: &[
                            &[T("__textrange1"), T("b")],
                            &[T("__textrange1_1"), T("b")],
                            &[T("_textrange1"), T("m")],
                            &[T("_textrange2"), T("b")],
                            &[T("textrange1"), T("r")],
                            &[T("textrange2"), T("r")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT _textrange1(textrange2('a', 'z')) @> 'b'::text;",
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
                    query: "SELECT multirange_of_text(textrange2('a', 'Z'));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function multirange_of_text(textrange2) does not exist", position: 8, hint: "No function matches the given name and argument types. You might need to add explicit type casts.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_range_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "ANALYZE gathers range and multirange histograms, and the range estimators read them",
            set_up_script: &[
                "CREATE TABLE rs (id INT PRIMARY KEY, r INT4RANGE, m INT4MULTIRANGE, n NUMRANGE) WITH (autovacuum_enabled = false);",
                "INSERT INTO rs SELECT i, CASE WHEN i % 10 = 0 THEN 'empty' WHEN i % 13 = 0 THEN NULL ELSE int4range(i, i + i % 7 + 1) END, CASE WHEN i % 11 = 0 THEN '{}' ELSE int4multirange(int4range(i, i + 3), int4range(i + 5, i + 5 + i % 4)) END, numrange(i / 3.0, CASE WHEN i % 17 = 0 THEN NULL ELSE i / 3.0 + 2 END) FROM generate_series(1, 2000) i;",
                "ANALYZE rs;",
                "CREATE FUNCTION est(q TEXT) RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE j JSON; BEGIN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j; RETURN j->0->'Plan'->>'Plan Rows'; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT attname, null_frac, avg_width > 0 AS has_width, n_distinct, array_length(range_bounds_histogram::text::text[], 1) AS bounds, range_empty_frac, array_length(range_length_histogram::text::text[], 1) AS lengths FROM pg_stats WHERE tablename = 'rs' AND attname IN ('r', 'm', 'n') ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("null_frac", FLOAT4), Column("has_width", BOOL), Column("n_distinct", FLOAT4), Column("bounds", INT4), Column("range_empty_frac", FLOAT4), Column("lengths", INT4)],
                        rows: &[
                            &[T("m"), T("0"), T("t"), T("-1"), T("101"), T("0.0905"), T("101")],
                            &[T("n"), T("0"), T("t"), T("-1"), T("101"), T("0"), T("101")],
                            &[T("r"), T("0.069"), T("t"), T("-0.931"), T("101"), T("0.107411385"), T("101")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (range_length_histogram::text::float8[])[1:5], (range_bounds_histogram::text::text[])[1:3] FROM pg_stats WHERE tablename = 'rs' AND attname = 'r';",
                    expected: Expected::Rows {
                        columns: &[Column("range_length_histogram", FLOAT8_ARRAY), Column("range_bounds_histogram", TEXT_ARRAY)],
                        rows: &[
                            &[T("{1,1,1,1,1}"), T("{\"[1,3)\",\"[19,23)\",\"[41,43)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (range_bounds_histogram::text::text[])[1:3] FROM pg_stats WHERE tablename = 'rs' AND attname = 'm';",
                    expected: Expected::Rows {
                        columns: &[Column("range_bounds_histogram", TEXT_ARRAY)],
                        rows: &[
                            &[T("{\"[1,7)\",\"[20,27)\",\"[40,47)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM rs WHERE r && ''[100,200)'''), est('SELECT * FROM rs WHERE r @> 150'), est('SELECT * FROM rs WHERE r <@ ''[100,200)'''), est('SELECT * FROM rs WHERE r @> ''[100,103)''');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("86"), T("4"), T("280"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM rs WHERE r < ''[500,501)'''), est('SELECT * FROM rs WHERE r >= ''[500,501)'''), est('SELECT * FROM rs WHERE r << ''[500,501)'''), est('SELECT * FROM rs WHERE r >> ''[500,501)'''), est('SELECT * FROM rs WHERE r &< ''[500,501)'''), est('SELECT * FROM rs WHERE r &> ''[500,501)''');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("415"), T("1247"), T("412"), T("1247"), T("412"), T("1247")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM rs WHERE r <@ ''[1000,)'''), est('SELECT * FROM rs WHERE r <@ ''(,1000)'''), est('SELECT * FROM rs WHERE r && ''empty'''), est('SELECT * FROM rs WHERE r <@ ''empty'''), est('SELECT * FROM rs WHERE ''[100,200)'' @> r');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("1030"), T("1028"), T("1"), T("200"), T("280")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM rs WHERE m && ''[100,200)''::int4range'), est('SELECT * FROM rs WHERE m @> 150'), est('SELECT * FROM rs WHERE m <@ ''{[100,200)}'''), est('SELECT * FROM rs WHERE m @> ''{[100,102)}'''), est('SELECT * FROM rs WHERE m < ''{[500,501)}'''), est('SELECT * FROM rs WHERE m << ''[500,501)''::int4range'), est('SELECT * FROM rs WHERE m &> ''{[500,501)}''');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("96"), T("6"), T("266"), T("4"), T("455"), T("448"), T("1364")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM rs WHERE n && numrange(100, 200)'), est('SELECT * FROM rs WHERE n @> 150.5'), est('SELECT * FROM rs WHERE n <@ numrange(100, 200)'), est('SELECT * FROM rs WHERE n @> numrange(100, 101)');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("323"), T("32"), T("278"), T("19")],
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
fn test_range_agg_compares_in_the_session() {
    run_scripts(&[
        ScriptTest {
            name: "range_agg over a range type whose subtype class compares by a SQL function",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(a INT, b INT) RETURNS INT IMMUTABLE LANGUAGE sql AS $$ SELECT btint4cmp(b, a) $$;",
                "CREATE OPERATOR CLASS int4_rev_ops FOR TYPE INT USING btree AS OPERATOR 1 >, OPERATOR 2 >=, OPERATOR 3 =, OPERATOR 4 <=, OPERATOR 5 <, FUNCTION 1 rev_cmp(INT, INT);",
                "CREATE TYPE revrange AS RANGE (subtype = int4, subtype_opclass = int4_rev_ops);",
                "CREATE TABLE ra (g INT, r revrange);",
                "INSERT INTO ra VALUES (1, revrange(9, 5)), (1, revrange(6, 2)), (1, revrange(20, 15)), (2, revrange(3, 1)), (2, NULL), (3, revrange(4, 4));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT g, range_agg(r)::text FROM ra GROUP BY g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("range_agg", TEXT)],
                        rows: &[
                            &[T("1"), T("{[20,15),[9,2)}")],
                            &[T("2"), T("{[3,1)}")],
                            &[T("3"), T("{}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT range_agg(r)::text FROM ra;",
                    expected: Expected::Rows {
                        columns: &[Column("range_agg", TEXT)],
                        rows: &[
                            &[T("{[20,15),[9,1)}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, range_intersect_agg(r)::text FROM ra WHERE r IS NOT NULL GROUP BY g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("range_intersect_agg", TEXT)],
                        rows: &[
                            &[T("1"), T("empty")],
                            &[T("2"), T("[3,1)")],
                            &[T("3"), T("empty")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT range_agg(m)::text FROM (SELECT range_agg(r) AS m FROM ra GROUP BY g) s;",
                    expected: Expected::Rows {
                        columns: &[Column("range_agg", TEXT)],
                        rows: &[
                            &[T("{[20,15),[9,1)}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT range_agg(r ORDER BY g)::text FROM ra WHERE g = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("range_agg", TEXT)],
                        rows: &[
                            &[T("{[20,15),[9,2)}")],
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
