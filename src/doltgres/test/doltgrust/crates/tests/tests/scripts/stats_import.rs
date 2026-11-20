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
fn test_stats_import() {
    run_scripts(&[
        ScriptTest {
            name: "pg_restore_attribute_stats merges into the statistics ANALYZE gathered",
            set_up_script: &[
                "CREATE TABLE sm (id INT PRIMARY KEY, a INT, b INT[]);",
                "INSERT INTO sm SELECT g, g % 10, ARRAY[g % 3, g % 5] FROM generate_series(1, 1000) g;",
                "ANALYZE sm;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT null_frac, avg_width, n_distinct, most_common_vals, most_common_freqs, histogram_bounds, correlation FROM pg_stats WHERE tablename = 'sm' AND attname = 'a';",
                    expected: Expected::Rows {
                        columns: &[Column("null_frac", FLOAT4), Column("avg_width", INT4), Column("n_distinct", FLOAT4), Column("most_common_vals", ANYARRAY), Column("most_common_freqs", FLOAT4_ARRAY), Column("histogram_bounds", ANYARRAY), Column("correlation", FLOAT4)],
                        rows: &[
                            &[T("0"), T("4"), T("10"), T("{0,1,2,3,4,5,6,7,8,9}"), T("{0.1,0.1,0.1,0.1,0.1,0.1,0.1,0.1,0.1,0.1}"), Null, T("0.1044901")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT most_common_elems, most_common_elem_freqs, elem_count_histogram FROM pg_stats WHERE tablename = 'sm' AND attname = 'b';",
                    expected: Expected::Rows {
                        columns: &[Column("most_common_elems", ANYARRAY), Column("most_common_elem_freqs", FLOAT4_ARRAY), Column("elem_count_histogram", FLOAT4_ARRAY)],
                        rows: &[
                            &[T("{0,1,2,3,4}"), T("{0.467,0.467,0.466,0.2,0.2,0.2,0.467,0}"), T("{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1.8}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', 'sm', 'attname', 'a', 'inherited', false, 'most_common_vals', '{1,2}', 'most_common_freqs', '{0.5,0.4}'::REAL[]);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_attribute_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT null_frac, avg_width, n_distinct, most_common_vals, most_common_freqs, histogram_bounds, correlation FROM pg_stats WHERE tablename = 'sm' AND attname = 'a';",
                    expected: Expected::Rows {
                        columns: &[Column("null_frac", FLOAT4), Column("avg_width", INT4), Column("n_distinct", FLOAT4), Column("most_common_vals", ANYARRAY), Column("most_common_freqs", FLOAT4_ARRAY), Column("histogram_bounds", ANYARRAY), Column("correlation", FLOAT4)],
                        rows: &[
                            &[T("0"), T("4"), T("10"), T("{1,2}"), T("{0.5,0.4}"), Null, T("0.1044901")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', 'sm', 'attname', 'a', 'inherited', false, 'histogram_bounds', '{1,5,9}', 'null_frac', 0.25::REAL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_attribute_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT null_frac, avg_width, n_distinct, most_common_vals, most_common_freqs, histogram_bounds, correlation FROM pg_stats WHERE tablename = 'sm' AND attname = 'a';",
                    expected: Expected::Rows {
                        columns: &[Column("null_frac", FLOAT4), Column("avg_width", INT4), Column("n_distinct", FLOAT4), Column("most_common_vals", ANYARRAY), Column("most_common_freqs", FLOAT4_ARRAY), Column("histogram_bounds", ANYARRAY), Column("correlation", FLOAT4)],
                        rows: &[
                            &[T("0.25"), T("4"), T("10"), T("{1,2}"), T("{0.5,0.4}"), T("{1,5,9}"), T("0.1044901")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stakind1, stakind2, stakind3, stakind4, stakind5, staop1, staop2, staop3, stanumbers1, stanumbers2, stanumbers3, stavalues1::TEXT, stavalues2::TEXT, stavalues3::TEXT FROM pg_statistic WHERE starelid = 'sm'::REGCLASS AND staattnum = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("stakind1", INT2), Column("stakind2", INT2), Column("stakind3", INT2), Column("stakind4", INT2), Column("stakind5", INT2), Column("staop1", OID), Column("staop2", OID), Column("staop3", OID), Column("stanumbers1", FLOAT4_ARRAY), Column("stanumbers2", FLOAT4_ARRAY), Column("stanumbers3", FLOAT4_ARRAY), Column("stavalues1", TEXT), Column("stavalues2", TEXT), Column("stavalues3", TEXT)],
                        rows: &[
                            &[T("1"), T("3"), T("2"), T("0"), T("0"), T("96"), T("97"), T("97"), T("{0.5,0.4}"), T("{0.1044901}"), Null, T("{1,2}"), Null, T("{1,5,9}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', 'sm', 'attname', 'b', 'inherited', false, 'most_common_elems', '{1,2}', 'most_common_elem_freqs', '{0.5,0.4,0.1,0.6,0.05}'::REAL[]);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_attribute_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT most_common_elems, most_common_elem_freqs, elem_count_histogram FROM pg_stats WHERE tablename = 'sm' AND attname = 'b';",
                    expected: Expected::Rows {
                        columns: &[Column("most_common_elems", ANYARRAY), Column("most_common_elem_freqs", FLOAT4_ARRAY), Column("elem_count_histogram", FLOAT4_ARRAY)],
                        rows: &[
                            &[T("{1,2}"), T("{0.5,0.4,0.1,0.6,0.05}"), T("{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1.8}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_clear_attribute_stats('public', 'sm', 'a', false);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_clear_attribute_stats", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_statistic WHERE starelid = 'sm'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_restore_relation_stats, the clear functions, and the argument checks",
            set_up_script: &[
                "CREATE TABLE sr (id INT PRIMARY KEY, a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'sr', 'relpages', 17::INTEGER, 'reltuples', 400::REAL, 'relallvisible', 4::INTEGER);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples, relallvisible, relallfrozen FROM pg_class WHERE oid = 'sr'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4), Column("relallvisible", INT4), Column("relallfrozen", INT4)],
                        rows: &[
                            &[T("17"), T("400"), T("4"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'sr', 'relallfrozen', 2::INTEGER, 'nope', 1::INTEGER);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"unrecognized argument name: "nope""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'sr', 'reltuples', -2::REAL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "22023", message: r#"argument "reltuples" must not be less than -1.0"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'sr', 'relpages', 'x'::TEXT);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"argument "relpages" has type text, expected type integer"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples, relallvisible, relallfrozen FROM pg_class WHERE oid = 'sr'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4), Column("relallvisible", INT4), Column("relallfrozen", INT4)],
                        rows: &[
                            &[T("17"), T("400"), T("4"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'sr', 'relpages');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "variadic arguments must be name/value pairs", hint: "Provide an even number of variadic arguments that can be divided into pairs.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'nosuch', 'relpages', 1::INTEGER);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "public.nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_clear_relation_stats('public', 'sr');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_clear_relation_stats", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples, relallvisible, relallfrozen FROM pg_class WHERE oid = 'sr'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4), Column("relallvisible", INT4), Column("relallfrozen", INT4)],
                        rows: &[
                            &[T("0"), T("-1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', 'sr', 'attname', 'a', 'inherited', false, 'most_common_vals', '{1,2}');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_attribute_stats", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "22023", message: r#"argument "most_common_freqs" must be specified when argument "most_common_vals" is specified"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', 'sr', 'attname', 'a', 'inherited', false, 'null_frac', 0.5::REAL, 'histogram_bounds', '{1,x}');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_attribute_stats", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "22P02", message: r#"invalid input syntax for type integer: "x""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT null_frac, avg_width, n_distinct, most_common_vals, histogram_bounds FROM pg_stats WHERE tablename = 'sr';",
                    expected: Expected::Rows {
                        columns: &[Column("null_frac", FLOAT4), Column("avg_width", INT4), Column("n_distinct", FLOAT4), Column("most_common_vals", ANYARRAY), Column("histogram_bounds", ANYARRAY)],
                        rows: &[
                            &[T("0.5"), T("0"), T("0"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', 'sr', 'attname', 'zz', 'inherited', false, 'null_frac', 0.5::REAL);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "zz" of relation "sr" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', 'sr', 'attname', 'a', 'attnum', 2::SMALLINT, 'inherited', false);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"cannot specify both "attname" and "attnum""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', 'sr', 'attname', 'ctid', 'inherited', false, 'null_frac', 0.5::REAL);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot modify statistics on system column "ctid""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_clear_attribute_stats('public', 'sr', 'a', false);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_clear_attribute_stats", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_statistic WHERE starelid = 'sr'::REGCLASS;",
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
            name: "restored relation statistics belong to the transaction, which ANALYZE's in-place statistics follow",
            set_up_script: &[
                "CREATE TABLE st (id INT PRIMARY KEY, a INT);",
                "INSERT INTO st SELECT g, g FROM generate_series(1, 100) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'st', 'relpages', 17::INTEGER, 'reltuples', 400::REAL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("17"), T("400")],
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
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("0"), T("-1")],
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
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'st', 'relpages', 17::INTEGER, 'reltuples', 400::REAL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE st;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("1"), T("100")],
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
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("1"), T("100")],
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
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'st', 'relpages', 18::INTEGER, 'reltuples', 500::REAL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE st;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("1"), T("100")],
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
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'st', 'relpages', 7::INTEGER, 'reltuples', 70::REAL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("t")],
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
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', 'st', 'relpages', 19::INTEGER, 'reltuples', 600::REAL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_restore_relation_stats", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE st;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("7"), T("70")],
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
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("7"), T("70")],
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
                    query: "SELECT pg_clear_relation_stats('public', 'st');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_clear_relation_stats", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("0"), T("-1")],
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
                    query: "SELECT relpages, reltuples FROM pg_class WHERE oid = 'st'::REGCLASS;",
                    expected: Expected::Rows {
                        columns: &[Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("7"), T("70")],
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
