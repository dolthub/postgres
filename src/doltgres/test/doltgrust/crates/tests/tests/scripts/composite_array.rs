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
fn test_arrays_of_composite_types() {
    run_scripts(&[
        ScriptTest {
            name: "arrays of named and anonymous composite types: input, output, storage, subscripts, comparison, and unnest",
            set_up_script: &[
                "CREATE TYPE cpair AS (a int, b text);",
                "CREATE TABLE ct (id int, ps cpair[]);",
                "INSERT INTO ct VALUES (1, ARRAY[ROW(1, 'x')::cpair, ROW(2, NULL)::cpair]), (2, '{\"(3,y)\",\"(4,\\\"a b\\\")\"}'), (3, NULL), (4, '{}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ARRAY[ROW(1, 'x')::cpair, ROW(2, 'y')::cpair];",
                    expected: Expected::Rows {
                        columns: &[Column("array", USER_DEFINED)],
                        rows: &[
                            &[T("{\"(1,x)\",\"(2,y)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY[ROW(1, 2), ROW(3, 4)];",
                    expected: Expected::Rows {
                        columns: &[Column("array", RECORD_ARRAY)],
                        rows: &[
                            &[T("{\"(1,2)\",\"(3,4)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{\"(1,x)\",\"(2,)\"}'::cpair[];",
                    expected: Expected::Rows {
                        columns: &[Column("cpair", USER_DEFINED)],
                        rows: &[
                            &[T("{\"(1,x)\",\"(2,)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, ps FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ps", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("{\"(1,x)\",\"(2,)\"}")],
                            &[T("2"), T("{\"(3,y)\",\"(4,\\\"a b\\\")\"}")],
                            &[T("3"), Null],
                            &[T("4"), T("{}")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, ps[1], ps[2].b, (ps[1]).a FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ps", USER_DEFINED), Column("b", TEXT), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("(1,x)"), Null, T("1")],
                            &[T("2"), T("(3,y)"), T("a b"), T("3")],
                            &[T("3"), Null, Null, Null],
                            &[T("4"), Null, Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, array_length(ps, 1), cardinality(ps) FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("array_length", INT4), Column("cardinality", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("2")],
                            &[T("2"), T("2"), T("2")],
                            &[T("3"), Null, Null],
                            &[T("4"), Null, T("0")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ct WHERE ps = ARRAY[ROW(1, 'x')::cpair, ROW(2, NULL)::cpair];",
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
                    query: "SELECT id FROM ct ORDER BY ps, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY[ROW(1, 'x')::cpair] < ARRAY[ROW(1, 'y')::cpair], ARRAY[ROW(2, 'a')::cpair] > ARRAY[ROW(1, 'z')::cpair];",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest((SELECT ps FROM ct WHERE id = 2));",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("3"), T("y")],
                            &[T("4"), T("a b")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT u.a, u.b FROM ct, unnest(ps) u WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_agg(ROW(id, id::text)::cpair ORDER BY id) FROM ct;",
                    expected: Expected::Rows {
                        columns: &[Column("array_agg", USER_DEFINED)],
                        rows: &[
                            &[T("{\"(1,1)\",\"(2,2)\",\"(3,3)\",\"(4,4)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_agg(ROW(id, 'z') ORDER BY id) FROM ct;",
                    expected: Expected::Rows {
                        columns: &[Column("array_agg", RECORD_ARRAY)],
                        rows: &[
                            &[T("{\"(1,z)\",\"(2,z)\",\"(3,z)\",\"(4,z)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ct SET ps[1] = ROW(9, 'nine') WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ps FROM ct WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("ps", USER_DEFINED)],
                        rows: &[
                            &[T("{\"(9,nine)\",\"(2,)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_append(ps, ROW(5, 'five')::cpair) FROM ct WHERE id = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("array_append", USER_DEFINED)],
                        rows: &[
                            &[T("{\"(5,five)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_typeof(ps), pg_typeof(ARRAY[ROW(1,2)]) FROM ct WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_typeof", REGTYPE), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("cpair[]"), T("record[]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY[ROW(1, 'x')::cpair] = ARRAY[ROW(1, 'x')::cpair];",
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
                    query: "SELECT '{\"(1,x)\"}'::record[];",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "input of anonymous composite types is not implemented", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY(SELECT ROW(id, ps[1].b)::cpair FROM ct WHERE id < 3 ORDER BY id);",
                    expected: Expected::Rows {
                        columns: &[Column("array", USER_DEFINED)],
                        rows: &[
                            &[T("{\"(1,nine)\",\"(2,y)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ps::text FROM ct WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("ps", TEXT)],
                        rows: &[
                            &[T("{\"(3,y)\",\"(4,\\\"a b\\\")\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{\"(1,x\"}'::cpair[];",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "malformed record literal: \"(1,x\"", position: 8, detail: "Unexpected end of input.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_expression_statistics_as_an_array_of_rows() {
    run_scripts(&[
        ScriptTest {
            name: "pg_statistic_ext_data.stxdexpr as a pg_statistic[] with each expression's statistics",
            set_up_script: &[
                "CREATE TABLE sx (a int, b int) WITH (autovacuum_enabled = false);",
                "INSERT INTO sx SELECT i % 5, i % 7 FROM generate_series(1, 100) i;",
                "CREATE STATISTICS sx_st ON (a + b), (a * 2) FROM sx;",
                "ANALYZE sx;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT array_length(d.stxdexpr, 1), array_ndims(d.stxdexpr), pg_typeof(d.stxdexpr) FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid WHERE s.stxname = 'sx_st';",
                    expected: Expected::Rows {
                        columns: &[Column("array_length", INT4), Column("array_ndims", INT4), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("2"), T("1"), T("pg_statistic[]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.starelid, e.staattnum, e.stainherit, e.stanullfrac, e.stawidth, e.stadistinct, e.stakind1, e.stakind2, e.stakind3 FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid, unnest(d.stxdexpr) e WHERE s.stxname = 'sx_st';",
                    expected: Expected::Rows {
                        columns: &[Column("starelid", OID), Column("staattnum", INT2), Column("stainherit", BOOL), Column("stanullfrac", FLOAT4), Column("stawidth", INT4), Column("stadistinct", FLOAT4), Column("stakind1", INT2), Column("stakind2", INT2), Column("stakind3", INT2)],
                        rows: &[
                            &[T("0"), T("0"), T("f"), T("0"), T("4"), T("-0.11"), T("1"), T("3"), T("0")],
                            &[T("0"), T("0"), T("f"), T("0"), T("4"), T("5"), T("1"), T("3"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (d.stxdexpr[2]).stavalues1::text, (d.stxdexpr[2]).stanumbers1 FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON d.stxoid = s.oid WHERE s.stxname = 'sx_st';",
                    expected: Expected::Rows {
                        columns: &[Column("stavalues1", TEXT), Column("stanumbers1", FLOAT4_ARRAY)],
                        rows: &[
                            &[T("{0,2,4,6,8}"), T("{0.2,0.2,0.2,0.2,0.2}")],
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
