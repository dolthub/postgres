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

//! Dolt's go/libraries/doltcore/sqle/enginetest/row_lookup_join_test.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_row_lookup_join_conversion_tests() {
    run_scripts(&[
        ScriptTest {
            name: "signed narrowing",
            intent: "A lookup join from bigint values into a narrower integer index finds only equal values.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v bigint);",
                "CREATE TABLE rdst (id int PRIMARY KEY, v smallint); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,-129),(2,-128),(3,0),(4,127),(5,128),(6,NULL),(7,1);",
                "INSERT INTO rdst VALUES (1,-128),(2,0),(3,127),(4,NULL),(5,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), T("3")],
                            &[T("7"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), T("3")],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), T("5")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), T("3")],
                            &[T("5"), Null],
                            &[T("6"), T("4")],
                            &[T("7"), T("5")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("2")],
                            &[T("4"), T("3")],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), T("5")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "signed to unsigned",
            intent: "A lookup join from bigint values into an index of MySQL's unsigned tinyint, here smallint, finds the matching values.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v bigint);",
                "CREATE TABLE rdst (id int PRIMARY KEY, v smallint); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,-1),(2,0),(3,255),(4,256),(5,NULL),(6,1);",
                "INSERT INTO rdst VALUES (1,0),(2,255),(3,NULL),(4,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("6"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), T("4")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("1")],
                            &[T("3"), T("2")],
                            &[T("4"), Null],
                            &[T("5"), T("3")],
                            &[T("6"), T("4")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("2")],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), T("4")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unsigned to signed",
            intent: "A lookup join from MySQL's unsigned bigint values, here numeric, into a bigint index finds only the values bigint can hold.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v numeric(20));",
                "CREATE TABLE rdst (id int PRIMARY KEY, v bigint); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,0),(2,9223372036854775807),(3,9223372036854775808),(4,18446744073709551615),(5,NULL),(6,1);",
                "INSERT INTO rdst VALUES (1,-1),(2,0),(3,9223372036854775807),(4,NULL),(5,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("6"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("4")],
                            &[T("6"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), T("5")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "integer widening",
            intent: "A lookup join from narrow integers into a wider integer index finds every matching value.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v int);",
                "CREATE TABLE rdst (id int PRIMARY KEY, v bigint); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,-2147483648),(2,2147483647),(3,NULL),(4,1);",
                "INSERT INTO rdst VALUES (1,-2147483648),(2,2147483647),(3,2147483648),(4,NULL),(5,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("4"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("4")],
                            &[T("4"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fractional decimal to integer",
            intent: "A lookup join from fractional decimals into an integer index matches only whole values.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v DECIMAL(12,3));",
                "CREATE TABLE rdst (id int PRIMARY KEY, v int); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,-1.5),(2,-1),(3,0.4),(4,0.5),(5,1),(6,1.5),(7,2),(8,NULL);",
                "INSERT INTO rdst VALUES (1,-2),(2,-1),(3,0),(4,1),(5,2),(6,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("5"), T("4")],
                            &[T("7"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("4")],
                            &[T("6"), Null],
                            &[T("7"), T("5")],
                            &[T("8"), Null],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("4")],
                            &[T("6"), Null],
                            &[T("7"), T("5")],
                            &[T("8"), T("6")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("4")],
                            &[T("6"), Null],
                            &[T("7"), T("5")],
                            &[T("8"), Null],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), Null],
                            &[T("8"), Null],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "fractional float to integer",
            intent: "A lookup join from fractional floats into an integer index matches only whole values.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v double precision);",
                "CREATE TABLE rdst (id int PRIMARY KEY, v int); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,-1.5),(2,0.5),(3,1),(4,1.49),(5,1.5),(6,2),(7,NULL);",
                "INSERT INTO rdst VALUES (1,-2),(2,0),(3,1),(4,2),(5,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("6"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("3")],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), T("4")],
                            &[T("7"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("3")],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), T("4")],
                            &[T("7"), T("5")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("3")],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), T("4")],
                            &[T("7"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "string to integer",
            intent: "A lookup join from strings into an integer index compares as Postgres allows.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v VARCHAR(40) );",
                "CREATE TABLE rdst (id int PRIMARY KEY, v int); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,'1'),(2,'01'),(3,'1x'),(4,'1.0'),(5,'1e0'),(6,'abc'),(7,''),(8,' 2 '),(9,NULL);",
                "INSERT INTO rdst VALUES (1,0),(2,1),(3,2),(4,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: integer = character varying", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 121, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: integer = character varying", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 126, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: integer = character varying", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 126, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: integer = character varying", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 126, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: integer = character varying", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 126, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "string width",
            intent: "A lookup join from long strings into a narrower varchar index matches only equal strings.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v VARCHAR(40) );",
                "CREATE TABLE rdst (id int PRIMARY KEY, v VARCHAR(3) ); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,'a'),(2,'abc'),(3,'abcd'),(4,''),(5,'é'),(6,NULL),(7,'b');",
                "INSERT INTO rdst VALUES (1,'a'),(2,'abc'),(3,''),(4,'é'),(5,NULL),(6,'b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("4"), T("3")],
                            &[T("5"), T("4")],
                            &[T("7"), T("6")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), T("3")],
                            &[T("5"), T("4")],
                            &[T("6"), Null],
                            &[T("7"), T("6")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), T("3")],
                            &[T("5"), T("4")],
                            &[T("6"), T("5")],
                            &[T("7"), T("6")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                            &[T("4"), T("3")],
                            &[T("5"), T("4")],
                            &[T("6"), Null],
                            &[T("7"), T("6")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "collation",
            intent: "A lookup join between strings compares them under the columns' collation.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v VARCHAR(20) );",
                "CREATE TABLE rdst (id int PRIMARY KEY, v VARCHAR(20) ); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,'ALPHA'),(2,'é'),(3,'a'),(4,'missing'),(5,NULL);",
                "INSERT INTO rdst VALUES (1,'alpha'),(2,'e'),(3,'A'),(4,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("MySQL's default collation compares strings ignoring case and accents, where Postgres' default collation tells them apart")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("MySQL's default collation compares strings ignoring case and accents, where Postgres' default collation tells them apart")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("4")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("MySQL's default collation compares strings ignoring case and accents, where Postgres' default collation tells them apart")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("MySQL's default collation compares strings ignoring case and accents, where Postgres' default collation tells them apart")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "decimal rescale",
            intent: "A lookup join between decimals of different scales matches equal values.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v DECIMAL(12,3));",
                "CREATE TABLE rdst (id int PRIMARY KEY, v DECIMAL(5,2)); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,1.234),(2,1.235),(3,1.230),(4,-1.235),(5,999.999),(6,NULL),(7,1.2);",
                "INSERT INTO rdst VALUES (1,1.23),(2,1.24),(3,-1.24),(4,999.99),(5,NULL),(6,1.2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("3"), T("1")],
                            &[T("7"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("1")],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), T("6")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), T("1")],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), T("5")],
                            &[T("7"), T("6")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), T("6")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                            &[T("7"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "date to datetime",
            intent: "A lookup join from dates into a timestamp index matches midnight timestamps.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v DATE);",
                "CREATE TABLE rdst (id int PRIMARY KEY, v timestamp(6)); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,'2020-01-01'),(2,'2020-01-02'),(3,NULL);",
                "INSERT INTO rdst VALUES (1,'2020-01-01 00:00:00'),(2,'2020-01-01 12:00:00'),(3,'2020-01-02 00:00:00'),(4,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("3")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("3")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("3")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "datetime to date",
            intent: "A lookup join from timestamps into a date index matches only midnight timestamps.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v timestamp(6));",
                "CREATE TABLE rdst (id int PRIMARY KEY, v DATE); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,'2020-01-01 00:00:00'),(2,'2020-01-01 12:00:00'),(3,'2020-01-02 00:00:00.000001'),(4,NULL),(5,'2020-01-02 00:00:00');",
                "INSERT INTO rdst VALUES (1,'2020-01-01'),(2,'2020-01-02'),(3,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("5"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), T("3")],
                            &[T("5"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "enum to string",
            intent: "A lookup join from MySQL's enum values, here text, into a varchar index matches equal strings.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v text);",
                "CREATE TABLE rdst (id int PRIMARY KEY, v VARCHAR(20)); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,'a'),(2,'b'),(3,'c'),(4,NULL);",
                "INSERT INTO rdst VALUES (1,'a'),(2,'b'),(3,'c'),(4,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "integer to string",
            intent: "A lookup join from integers into a varchar index compares as Postgres allows.",
            set_up_script: &[
                "CREATE TABLE lsrc (id int PRIMARY KEY, v int);",
                "CREATE TABLE rdst (id int PRIMARY KEY, v VARCHAR(20)); CREATE INDEX v_idx ON rdst (v);",
                "INSERT INTO lsrc VALUES (1,0),(2,1),(3,2),(4,NULL);",
                "INSERT INTO rdst VALUES (1,'1'),(2,'01'),(3,'1.0'),(4,'abc'),(5,'2'),(6,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nJOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 121, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 126, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v IS NOT DISTINCT FROM l.v\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 126, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id > 1\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 126, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id, r.id\nFROM (SELECT id, v FROM lsrc LIMIT 1000) l\nLEFT JOIN rdst r ON r.v = l.v AND r.id < 0\nORDER BY l.id, r.id;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 126, ..E }),
                    dolt: Some(Dolt::Differs("MySQL compares a string with a number as numbers, where Postgres has no operator between varchar and integer")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "composite keys and outer join state",
            intent: "Lookup joins on composite keys keep correct outer join rows across duplicates, NULLs, and extra conditions.",
            set_up_script: &[
                "CREATE TABLE sources (id int PRIMARY KEY, a bigint, b bigint);",
                "CREATE TABLE targets (id int PRIMARY KEY, a smallint, b smallint, payload VARCHAR(10)); CREATE INDEX ab ON targets (a,b);",
                "INSERT INTO sources VALUES (1,1,2),(2,1,300),(3,1,NULL),(4,NULL,2),(5,1,2),(6,9,9);",
                "INSERT INTO targets VALUES (1,1,2,'keep'),(2,1,2,'drop'),(3,1,NULL,'nullb'),(4,NULL,2,'nulla');",
                "CREATE TABLE duplicates (a smallint, b smallint, payload VARCHAR(10)); CREATE INDEX duplicates_ab ON duplicates (a,b);",
                "INSERT INTO duplicates VALUES (1,2,'keep'),(1,2,'keep'),(1,2,'drop'),(1,NULL,'nullb');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN targets r ON r.a=l.a AND r.b=l.b\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("drop")],
                            &[T("1"), T("keep")],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("drop")],
                            &[T("5"), T("keep")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN targets r ON r.a IS NOT DISTINCT FROM l.a AND r.b IS NOT DISTINCT FROM l.b\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("drop")],
                            &[T("1"), T("keep")],
                            &[T("2"), Null],
                            &[T("3"), T("nullb")],
                            &[T("4"), T("nulla")],
                            &[T("5"), T("drop")],
                            &[T("5"), T("keep")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN targets r ON r.a=l.a AND r.b=l.b AND r.payload='keep'\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("keep")],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("keep")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN targets r ON r.a=l.a AND r.b=l.b AND r.payload='missing'\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN targets r ON r.a=l.a AND r.b=2\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("drop")],
                            &[T("1"), T("keep")],
                            &[T("2"), T("drop")],
                            &[T("2"), T("keep")],
                            &[T("3"), T("drop")],
                            &[T("3"), T("keep")],
                            &[T("4"), Null],
                            &[T("5"), T("drop")],
                            &[T("5"), T("keep")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN targets r ON r.a=l.a\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("drop")],
                            &[T("1"), T("keep")],
                            &[T("1"), T("nullb")],
                            &[T("2"), T("drop")],
                            &[T("2"), T("keep")],
                            &[T("2"), T("nullb")],
                            &[T("3"), T("drop")],
                            &[T("3"), T("keep")],
                            &[T("3"), T("nullb")],
                            &[T("4"), Null],
                            &[T("5"), T("drop")],
                            &[T("5"), T("keep")],
                            &[T("5"), T("nullb")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 14",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN duplicates r ON r.a=l.a AND r.b=l.b\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("drop")],
                            &[T("1"), T("keep")],
                            &[T("1"), T("keep")],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("drop")],
                            &[T("5"), T("keep")],
                            &[T("5"), T("keep")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN duplicates r ON r.a IS NOT DISTINCT FROM l.a AND r.b IS NOT DISTINCT FROM l.b\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("drop")],
                            &[T("1"), T("keep")],
                            &[T("1"), T("keep")],
                            &[T("2"), Null],
                            &[T("3"), T("nullb")],
                            &[T("4"), Null],
                            &[T("5"), T("drop")],
                            &[T("5"), T("keep")],
                            &[T("5"), T("keep")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN duplicates r ON r.a=l.a AND r.b=l.b AND r.payload='keep'\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("keep")],
                            &[T("1"), T("keep")],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), T("keep")],
                            &[T("5"), T("keep")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN duplicates r ON r.a=l.a AND r.b=l.b AND r.payload='missing'\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), Null],
                            &[T("5"), Null],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN duplicates r ON r.a=l.a AND r.b=2\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("drop")],
                            &[T("1"), T("keep")],
                            &[T("1"), T("keep")],
                            &[T("2"), T("drop")],
                            &[T("2"), T("keep")],
                            &[T("2"), T("keep")],
                            &[T("3"), T("drop")],
                            &[T("3"), T("keep")],
                            &[T("3"), T("keep")],
                            &[T("4"), Null],
                            &[T("5"), T("drop")],
                            &[T("5"), T("keep")],
                            &[T("5"), T("keep")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 14",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.payload\nFROM (SELECT * FROM sources LIMIT 1000) l\nLEFT JOIN duplicates r ON r.a=l.a\nORDER BY l.id, r.payload;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("payload", VARCHAR)],
                        rows: &[
                            &[T("1"), T("drop")],
                            &[T("1"), T("keep")],
                            &[T("1"), T("keep")],
                            &[T("1"), T("nullb")],
                            &[T("2"), T("drop")],
                            &[T("2"), T("keep")],
                            &[T("2"), T("keep")],
                            &[T("2"), T("nullb")],
                            &[T("3"), T("drop")],
                            &[T("3"), T("keep")],
                            &[T("3"), T("keep")],
                            &[T("3"), T("nullb")],
                            &[T("4"), Null],
                            &[T("5"), T("drop")],
                            &[T("5"), T("keep")],
                            &[T("5"), T("keep")],
                            &[T("5"), T("nullb")],
                            &[T("6"), Null],
                        ],
                        tag: "SELECT 18",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH grouped AS (SELECT a, COUNT(*) AS n FROM sources GROUP BY a)\nSELECT /*+ LOOKUP_JOIN(g,r) */ g.a, g.n, r.id\nFROM grouped g LEFT JOIN targets r ON r.a=g.a\nORDER BY g.a, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8), Column("n", INT8), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("1")],
                            &[T("1"), T("4"), T("2")],
                            &[T("1"), T("4"), T("3")],
                            &[T("9"), T("1"), Null],
                            &[Null, T("1"), Null],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::Differs("MySQL sorts NULL first in ascending order, where Postgres sorts it last")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) */ l.id, r.id\nFROM (SELECT * FROM sources LIMIT 1000) l\nJOIN targets r ON r.a=l.a AND r.b=l.b\nORDER BY l.id, r.id LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multi-table lookup join shapes",
            intent: "Lookup joins chained across several tables return the same rows as plain joins.",
            set_up_script: &[
                "CREATE TABLE shape_src (id int PRIMARY KEY, v bigint);",
                "CREATE TABLE shape_mid (id int PRIMARY KEY, v smallint);",
                "CREATE INDEX mid_v ON shape_mid(v);",
                "CREATE TABLE shape_tail (id int PRIMARY KEY, v int);",
                "CREATE INDEX tail_v ON shape_tail(v);",
                "CREATE TABLE shape_bridge (v smallint, tail_id int, payload int, PRIMARY KEY(v,tail_id));",
                "INSERT INTO shape_src VALUES (1,1),(2,2),(3,32768),(4,NULL),(5,3),(6,1),(7,9);",
                "INSERT INTO shape_mid VALUES (11,1),(12,1),(13,2),(14,NULL),(15,3);",
                "INSERT INTO shape_tail VALUES (21,11),(22,11),(23,13),(24,NULL),(25,99);",
                "INSERT INTO shape_bridge VALUES (1,21,101),(1,22,102),(2,23,103);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r,q) */ l.id, r.id, q.id\nFROM (SELECT * FROM shape_src LIMIT 100) l\nJOIN shape_mid r ON r.v = l.v\nJOIN (SELECT * FROM shape_tail LIMIT 100) q ON q.v = r.id\nORDER BY l.id, r.id, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("21")],
                            &[T("1"), T("11"), T("22")],
                            &[T("2"), T("13"), T("23")],
                            &[T("6"), T("11"), T("21")],
                            &[T("6"), T("11"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r,q) */ l.id, r.id, q.id\nFROM (SELECT * FROM shape_src LIMIT 100) l\nLEFT JOIN shape_mid r ON r.v = l.v\nLEFT JOIN (SELECT * FROM shape_tail LIMIT 100) q ON q.v = r.id\nORDER BY l.id, r.id, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("21")],
                            &[T("1"), T("11"), T("22")],
                            &[T("1"), T("12"), Null],
                            &[T("2"), T("13"), T("23")],
                            &[T("3"), Null, Null],
                            &[T("4"), Null, Null],
                            &[T("5"), T("15"), Null],
                            &[T("6"), T("11"), T("21")],
                            &[T("6"), T("11"), T("22")],
                            &[T("6"), T("12"), Null],
                            &[T("7"), Null, Null],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) LOOKUP_JOIN(r,q) JOIN_ORDER(l,r,q) */ l.id, r.id, q.id\nFROM (SELECT * FROM shape_src LIMIT 100) l\nLEFT JOIN shape_mid r ON r.v = l.v\nLEFT JOIN shape_tail q ON q.v = r.id\nORDER BY l.id, r.id, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("21")],
                            &[T("1"), T("11"), T("22")],
                            &[T("1"), T("12"), Null],
                            &[T("2"), T("13"), T("23")],
                            &[T("3"), Null, Null],
                            &[T("4"), Null, Null],
                            &[T("5"), T("15"), Null],
                            &[T("6"), T("11"), T("21")],
                            &[T("6"), T("11"), T("22")],
                            &[T("6"), T("12"), Null],
                            &[T("7"), Null, Null],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,q,r) */ l.id, q.id, r.id\nFROM (SELECT * FROM shape_src LIMIT 100) l\nJOIN (SELECT * FROM shape_src LIMIT 100) q ON q.v = l.v AND q.id > l.id\nLEFT JOIN shape_mid r ON r.v = q.v\nORDER BY l.id, q.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("6"), T("11")],
                            &[T("1"), T("6"), T("12")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(j,q) JOIN_ORDER(j,q) */ j.lid, j.rid, q.id\nFROM (\n SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id AS lid, r.id AS rid\n FROM (SELECT * FROM shape_src LIMIT 100) l\n LEFT JOIN shape_mid r ON r.v = l.v\n LIMIT 100\n) j\nLEFT JOIN shape_tail q ON q.v = j.rid\nORDER BY j.lid, j.rid, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("lid", INT4), Column("rid", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("21")],
                            &[T("1"), T("11"), T("22")],
                            &[T("1"), T("12"), Null],
                            &[T("2"), T("13"), T("23")],
                            &[T("3"), Null, Null],
                            &[T("4"), Null, Null],
                            &[T("5"), T("15"), Null],
                            &[T("6"), T("11"), T("21")],
                            &[T("6"), T("11"), T("22")],
                            &[T("6"), T("12"), Null],
                            &[T("7"), Null, Null],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r,q) */ l.v, l.n, r.id, q.n\nFROM (SELECT v, COUNT(*) AS n FROM shape_src GROUP BY v) l\nLEFT JOIN shape_mid r ON r.v = l.v\nLEFT JOIN (SELECT v, COUNT(*) AS n FROM shape_tail GROUP BY v) q ON q.v = r.id\nORDER BY l.v, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT8), Column("n", INT8), Column("id", INT4), Column("n", INT8)],
                        rows: &[
                            &[T("1"), T("2"), T("11"), T("2")],
                            &[T("1"), T("2"), T("12"), Null],
                            &[T("2"), T("1"), T("13"), T("1")],
                            &[T("3"), T("1"), T("15"), Null],
                            &[T("9"), T("1"), Null, Null],
                            &[T("32768"), T("1"), Null, Null],
                            &[Null, T("1"), Null, Null],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::Differs("MySQL sorts NULL first in ascending order, where Postgres sorts it last")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r,q) */ l.id, r.id, q.id\nFROM (\n SELECT id, v FROM shape_src WHERE id <= 2\n UNION ALL\n SELECT id, v FROM shape_src WHERE id = 1\n) l\nLEFT JOIN shape_mid r ON r.v = l.v AND r.id <> 12\nLEFT JOIN (SELECT * FROM shape_tail WHERE id > 21 LIMIT 100) q ON q.v = r.id\nORDER BY l.id, r.id, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("22")],
                            &[T("1"), T("11"), T("22")],
                            &[T("2"), T("13"), T("23")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) LOOKUP_JOIN(r,b) JOIN_ORDER(l,r,q,b) */ l.id, r.id, q.id, b.payload\nFROM (SELECT * FROM shape_src LIMIT 100) l\nLEFT JOIN shape_mid r ON r.v = l.v\nLEFT JOIN (SELECT * FROM shape_tail LIMIT 100) q ON q.v = r.id\nLEFT JOIN shape_bridge b ON b.v = r.v AND b.tail_id = q.id\nORDER BY l.id, r.id, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4), Column("payload", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("21"), T("101")],
                            &[T("1"), T("11"), T("22"), T("102")],
                            &[T("1"), T("12"), Null, Null],
                            &[T("2"), T("13"), T("23"), T("103")],
                            &[T("3"), Null, Null, Null],
                            &[T("4"), Null, Null, Null],
                            &[T("5"), T("15"), Null, Null],
                            &[T("6"), T("11"), T("21"), T("101")],
                            &[T("6"), T("11"), T("22"), T("102")],
                            &[T("6"), T("12"), Null, Null],
                            &[T("7"), Null, Null, Null],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH s AS (SELECT * FROM shape_src LIMIT 100)\nSELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r,q) */ l.id, r.id, q.id\nFROM s l\nJOIN shape_mid r ON r.v = l.v\nJOIN s q ON q.v = r.v AND q.id > l.id\nORDER BY l.id, r.id, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("6")],
                            &[T("1"), T("12"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(j,q) JOIN_ORDER(j,q) */ j.lid, j.rid, q.id\nFROM (\n SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r) */ l.id AS lid, r.id AS rid, r.id + 10 AS next_id\n FROM (SELECT * FROM shape_src LIMIT 100) l\n LEFT JOIN shape_mid r ON r.v = l.v\n LIMIT 100\n) j\nLEFT JOIN shape_tail q ON q.id = j.next_id\nORDER BY j.lid, j.rid, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("lid", INT4), Column("rid", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("21")],
                            &[T("1"), T("12"), T("22")],
                            &[T("2"), T("13"), T("23")],
                            &[T("3"), Null, Null],
                            &[T("4"), Null, Null],
                            &[T("5"), T("15"), T("25")],
                            &[T("6"), T("11"), T("21")],
                            &[T("6"), T("12"), T("22")],
                            &[T("7"), Null, Null],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) LOOKUP_JOIN(r,q) JOIN_ORDER(l,r,q) */ l.id, r.id, q.id\nFROM (SELECT * FROM shape_src LIMIT 100) l\nLEFT JOIN shape_mid r ON r.v = l.v AND r.id < 0\nLEFT JOIN shape_tail q ON q.v = r.id\nORDER BY l.id, r.id, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), Null, Null],
                            &[T("2"), Null, Null],
                            &[T("3"), Null, Null],
                            &[T("4"), Null, Null],
                            &[T("5"), Null, Null],
                            &[T("6"), Null, Null],
                            &[T("7"), Null, Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) LOOKUP_JOIN(r,q) JOIN_ORDER(l,r,q) */ l.id, r.id, q.id\nFROM (SELECT * FROM shape_src LIMIT 100) l\nLEFT JOIN shape_mid r ON r.v = l.v\nJOIN shape_tail q ON q.v = r.id\nORDER BY l.id, r.id, q.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("21")],
                            &[T("1"), T("11"), T("22")],
                            &[T("2"), T("13"), T("23")],
                            &[T("6"), T("11"), T("21")],
                            &[T("6"), T("11"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(l,r) JOIN_ORDER(l,r,q) */ l.id, r.id, q.id\nFROM (SELECT * FROM shape_src LIMIT 100) l\nLEFT JOIN shape_mid r ON r.v = l.v\nLEFT JOIN (SELECT * FROM shape_tail LIMIT 100) q ON q.v = r.id\nORDER BY l.id, r.id, q.id LIMIT 3 OFFSET 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("22")],
                            &[T("1"), T("12"), Null],
                            &[T("2"), T("13"), T("23")],
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
