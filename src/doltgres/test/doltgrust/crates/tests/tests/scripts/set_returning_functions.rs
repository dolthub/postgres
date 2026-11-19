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
fn test_set_returning_functions_in_from() {
    run_scripts(&[
        ScriptTest {
            name: "set-returning functions in FROM",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 5);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 10, 3) AS g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("4")],
                            &[T("7")],
                            &[T("10")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, g * 2 AS doubled FROM generate_series(5, 1, -2) AS g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("doubled", INT4)],
                        rows: &[
                            &[T("5"), T("10")],
                            &[T("3"), T("6")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(2147483645, 2147483647);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                            &[T("2147483645")],
                            &[T("2147483646")],
                            &[T("2147483647")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(9223372036854775806::bigint, 9223372036854775807::bigint);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT8)],
                        rows: &[
                            &[T("9223372036854775806")],
                            &[T("9223372036854775807")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 3, 0);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "step size cannot equal zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(3, 1);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1.5, 3.5, 0.5);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", NUMERIC)],
                        rows: &[
                            &[T("1.5")],
                            &[T("2.0")],
                            &[T("2.5")],
                            &[T("3.0")],
                            &[T("3.5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(0.1, 0.3, 0.1);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", NUMERIC)],
                        rows: &[
                            &[T("0.1")],
                            &[T("0.2")],
                            &[T("0.3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series('NaN'::numeric, 3);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "start value cannot be NaN", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 'Infinity'::numeric);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "stop value cannot be infinity", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1::numeric, 3, 0);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "step size cannot equal zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series('2024-01-01'::timestamp, '2024-01-03', '1 day');",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", TIMESTAMP)],
                        rows: &[
                            &[T("2024-01-01 00:00:00")],
                            &[T("2024-01-02 00:00:00")],
                            &[T("2024-01-03 00:00:00")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series('2024-01-31'::timestamp, '2024-04-30', '1 month');",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", TIMESTAMP)],
                        rows: &[
                            &[T("2024-01-31 00:00:00")],
                            &[T("2024-02-29 00:00:00")],
                            &[T("2024-03-29 00:00:00")],
                            &[T("2024-04-29 00:00:00")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series('2024-01-01'::timestamp, '2024-01-02', '0 days');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "step size cannot equal zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series('2024-01-01'::timestamp, '2024-01-02', 'infinity'::interval);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "step size cannot be infinite", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TimeZone = 'UTC';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series('2024-03-09 12:00+00'::timestamptz, '2024-03-11 12:00+00', '1 day', 'America/New_York');",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", TIMESTAMPTZ)],
                        rows: &[
                            &[T("2024-03-09 12:00:00+00")],
                            &[T("2024-03-10 11:00:00+00")],
                            &[T("2024-03-11 11:00:00+00")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest(ARRAY['a', 'b', NULL, 'd']) WITH ORDINALITY AS u(x, n);",
                    expected: Expected::Rows {
                        columns: &[Column("x", TEXT), Column("n", INT8)],
                        rows: &[
                            &[T("a"), T("1")],
                            &[T("b"), T("2")],
                            &[Null, T("3")],
                            &[T("d"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest(ARRAY[[1, 2], [3, 4]]);",
                    expected: Expected::Rows {
                        columns: &[Column("unnest", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest('{}'::int[]);",
                    expected: Expected::Rows {
                        columns: &[Column("unnest", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest(ARRAY[1, 2], ARRAY['x', 'y', 'z']) AS u(n, s);",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("s", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[Null, T("z")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ROWS FROM (generate_series(1, 3), unnest(ARRAY[10, 20])) AS r(a, b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ROWS FROM (generate_series(1, 2), generate_series(5, 7)) WITH ORDINALITY AS r(a, b, o);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("1"), T("5"), T("1")],
                            &[T("2"), T("6"), T("2")],
                            &[Null, T("7"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_subscripts(ARRAY[[1, 2], [3, 4], [5, 6]], 1);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_subscripts", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_subscripts('[3:5]={a,b,c}'::text[], 1, true);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_subscripts", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("4")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_subscripts(ARRAY[1, 2], 2);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_subscripts", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM string_to_table('a,b,,c', ',', '');",
                    expected: Expected::Rows {
                        columns: &[Column("string_to_table", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[Null],
                            &[T("c")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM string_to_table('abc', NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("string_to_table", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM string_to_table(NULL, ',');",
                    expected: Expected::Rows {
                        columns: &[Column("string_to_table", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM regexp_split_to_table('hello world  foo', '\\s+');",
                    expected: Expected::Rows {
                        columns: &[Column("regexp_split_to_table", TEXT)],
                        rows: &[
                            &[T("hello")],
                            &[T("world")],
                            &[T("foo")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM regexp_split_to_table('abc', '');",
                    expected: Expected::Rows {
                        columns: &[Column("regexp_split_to_table", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM regexp_split_to_table('a1b2c', '\\d', 'g');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "regexp_split_to_table() does not support the \"global\" option", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM regexp_matches('foobarbequebaz', '(b..)', 'g');",
                    expected: Expected::Rows {
                        columns: &[Column("regexp_matches", TEXT_ARRAY)],
                        rows: &[
                            &[T("{bar}")],
                            &[T("{beq}")],
                            &[T("{baz}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM regexp_matches('foobarbequebaz', '(b)(a)');",
                    expected: Expected::Rows {
                        columns: &[Column("regexp_matches", TEXT_ARRAY)],
                        rows: &[
                            &[T("{b,a}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM regexp_matches('abc', 'z', 'g');",
                    expected: Expected::Rows {
                        columns: &[Column("regexp_matches", TEXT_ARRAY)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM upper('x');",
                    expected: Expected::Rows {
                        columns: &[Column("upper", TEXT)],
                        rows: &[
                            &[T("X")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 3) g WHERE g > 1;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM generate_series(1, 1000);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1000")],
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
fn test_set_returning_functions_in_target_list() {
    run_scripts(&[
        ScriptTest {
            name: "set-returning functions in the target list",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, n int, tags text[]);",
                "INSERT INTO t VALUES (1, 2, ARRAY['a', 'b']), (2, 0, ARRAY[]::text[]), (3, 3, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT generate_series(1, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT generate_series(1, 3), generate_series(1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4), Column("generate_series", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT generate_series(1, generate_series(1, 3));",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                            &[T("2")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unnest(ARRAY[1, 2, 3]) * 10 AS x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                            &[T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 + generate_series(1, 2), 'a' AS c;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("2"), T("a")],
                            &[T("3"), T("a")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT generate_series(1, 5) LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, generate_series(1, n) FROM t ORDER BY id, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("generate_series", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                            &[T("3"), T("1")],
                            &[T("3"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, unnest(tags) FROM t ORDER BY id, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("unnest", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("1"), T("b")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), generate_series(1, 3) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("generate_series", INT4)],
                        rows: &[
                            &[T("3"), T("1")],
                            &[T("3"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x, count(*) FROM (SELECT generate_series(1, 3) AS x FROM t) s GROUP BY x ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("2"), T("3")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT generate_series(1, 3) AS g ORDER BY g DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT unnest(ARRAY[3, 1, 3, 2]) AS u ORDER BY u;",
                    expected: Expected::Rows {
                        columns: &[Column("u", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT generate_series(1, n) AS g, count(*) FROM t GROUP BY n, g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT regexp_matches('abc', '(a)(b)'), unnest(ARRAY['x', 'y']);",
                    expected: Expected::Rows {
                        columns: &[Column("regexp_matches", TEXT_ARRAY), Column("unnest", TEXT)],
                        rows: &[
                            &[T("{a,b}"), T("x")],
                            &[Null, T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT generate_series(1, 3) + generate_series(1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("4")],
                            &[Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT CASE WHEN true THEN generate_series(1, 2) END;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "set-returning functions are not allowed in CASE", position: 28, hint: "You might be able to move the set-returning function into a LATERAL FROM item.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(generate_series(1, 2), 0);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "set-returning functions are not allowed in COALESCE", position: 17, hint: "You might be able to move the set-returning function into a LATERAL FROM item.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE generate_series(1, 2) > 1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "set-returning functions are not allowed in WHERE", position: 23, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(generate_series(1, 3));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "aggregate function calls cannot contain set-returning function calls", position: 12, hint: "You might be able to move the set-returning function into a LATERAL FROM item.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT generate_series(1, 2) FROM generate_series(1, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT generate_series(1, 3) AS a) s WHERE a > 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, (SELECT max(g) FROM generate_series(1, t.n) g) FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("max", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), Null],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_to_table('a b', ' '), regexp_split_to_table('1,2,3', ',');",
                    expected: Expected::Rows {
                        columns: &[Column("string_to_table", TEXT), Column("regexp_split_to_table", TEXT)],
                        rows: &[
                            &[T("a"), T("1")],
                            &[T("b"), T("2")],
                            &[Null, T("3")],
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
fn test_set_returning_functions_lateral_and_explain() {
    run_scripts(&[
        ScriptTest {
            name: "set-returning functions with LATERAL, in joins, and in EXPLAIN",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, n int) WITH (autovacuum_enabled = false);",
                "INSERT INTO t VALUES (1, 2), (2, 0), (3, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT t.id, g FROM t, LATERAL generate_series(1, t.n) g ORDER BY t.id, g;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                            &[T("3"), T("1")],
                            &[T("3"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t.id, g FROM t LEFT JOIN LATERAL generate_series(1, t.n) g ON true ORDER BY t.id, g;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                            &[T("2"), Null],
                            &[T("3"), T("1")],
                            &[T("3"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 3) a, LATERAL generate_series(1, a) b ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("1")],
                            &[T("3"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t.id, u.x, u.o FROM t, LATERAL unnest(ARRAY[t.id * 10, t.n]) WITH ORDINALITY AS u(x, o) ORDER BY t.id, u.o;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("x", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("1"), T("10"), T("1")],
                            &[T("1"), T("2"), T("2")],
                            &[T("2"), T("20"), T("1")],
                            &[T("2"), T("0"), T("2")],
                            &[T("3"), T("30"), T("1")],
                            &[T("3"), T("3"), T("2")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM generate_series(1, 2) a JOIN generate_series(2, 3) b ON a = b;",
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
                    query: "SELECT count(*) FROM generate_series(1, 3) a, generate_series(1, 4) b;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE n IN (SELECT * FROM generate_series(2, 3)) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, EXISTS (SELECT 1 FROM generate_series(1, t.n) g WHERE g = 2) FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("exists", BOOL)],
                        rows: &[
                            &[T("1"), T("t")],
                            &[T("2"), T("f")],
                            &[T("3"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM generate_series(1, 5) g WHERE g > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on generate_series g")],
                            &[T("  Filter: (g > 2)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM generate_series(1, 5) g;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on pg_catalog.generate_series g")],
                            &[T("  Output: g")],
                            &[T("  Function Call: generate_series(1, 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM generate_series(1, 100);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on generate_series  (cost=0.00..1.00 rows=100 width=4)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM generate_series(1, 10, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on generate_series  (cost=0.00..0.05 rows=5 width=4)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM unnest(ARRAY[1, 2, 3]);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on unnest  (cost=0.00..0.03 rows=3 width=4)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM generate_series('2024-01-01'::timestamp, '2024-01-11', '1 day');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on generate_series  (cost=0.00..0.11 rows=11 width=8)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM generate_series(1.0, 2.0, 0.25);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on generate_series  (cost=0.00..0.05 rows=5 width=32)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM unnest(ARRAY[1, 2]) WITH ORDINALITY AS u(x, o) ORDER BY o;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on unnest u")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT generate_series(1, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("ProjectSet")],
                            &[T("  ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT generate_series(1, generate_series(1, 3));",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("ProjectSet")],
                            &[T("  Output: generate_series(1, (generate_series(1, 3)))")],
                            &[T("  ->  ProjectSet")],
                            &[T("        Output: generate_series(1, 3)")],
                            &[T("        ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT generate_series(1, 10), unnest(ARRAY[1, 2]);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("ProjectSet  (cost=0.00..0.07 rows=10 width=8)")],
                            &[T("  ->  Result  (cost=0.00..0.01 rows=1 width=0)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id, generate_series(1, n) FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("ProjectSet")],
                            &[T("  ->  Index Scan using t_pkey on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT t.id, g FROM t, LATERAL generate_series(1, t.n) g;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop")],
                            &[T("  ->  Seq Scan on t")],
                            &[T("  ->  Function Scan on generate_series g")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ROWS FROM (generate_series(1, 3), unnest(ARRAY[10, 20])) WITH ORDINALITY AS r(a, b, o);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on r")],
                            &[T("  Output: a, b, o")],
                            &[T("  Function Call: generate_series(1, 3), unnest('{10,20}'::integer[])")],
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
fn test_set_returning_functions_scrolling_and_empty_inputs() {
    run_scripts(&[
        ScriptTest {
            name: "a scroll cursor over functions of different lengths, and a set-returning call over no rows",
            set_up_script: &[
                "CREATE TABLE few (id int, dataa text);",
                "INSERT INTO few VALUES (1, 'a'), (2, 'a'), (3, 'b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE rf_cur SCROLL CURSOR FOR SELECT * FROM ROWS FROM (generate_series(1, 5), generate_series(1, 2)) WITH ORDINALITY AS g(i, j, o);",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM rf_cur;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                            &[T("3"), Null, T("3")],
                            &[T("4"), Null, T("4")],
                            &[T("5"), Null, T("5")],
                        ],
                        tag: "FETCH 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM rf_cur;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("5"), Null, T("5")],
                            &[T("4"), Null, T("4")],
                            &[T("3"), Null, T("3")],
                            &[T("2"), T("2"), T("2")],
                            &[T("1"), T("1"), T("1")],
                        ],
                        tag: "FETCH 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM rf_cur;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                            &[T("3"), Null, T("3")],
                            &[T("4"), Null, T("4")],
                            &[T("5"), Null, T("5")],
                        ],
                        tag: "FETCH 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM rf_cur;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("o", INT8)],
                        rows: &[
                        ],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM rf_cur;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("5"), Null, T("5")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 2 FROM rf_cur;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 2 FROM rf_cur;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("4"), Null, T("4")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM rf_cur;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("o", INT8)],
                        rows: &[
                            &[T("3"), Null, T("3")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM few f1, (SELECT unnest(ARRAY[1, 2]) FROM few f2 WHERE false OFFSET 0) ss;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Result")],
                            &[T("  Output: f1.id, f1.dataa, ss.unnest")],
                            &[T("  One-Time Filter: false")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM few f1, (SELECT unnest(ARRAY[1, 2]) FROM few f2 WHERE false OFFSET 0) ss;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("dataa", TEXT), Column("unnest", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_sql_functions_returning_sets() {
    run_scripts(&[
        ScriptTest {
            name: "SQL functions returning sets, rows, and tables, inlined and not",
            set_up_script: &[
                "CREATE TABLE tt (a int, b text);",
                "INSERT INTO tt VALUES (1, 'x'), (2, 'y'), (3, 'z');",
                "CREATE FUNCTION fv(n int) RETURNS SETOF int LANGUAGE sql VOLATILE AS 'SELECT generate_series(1, n)';",
                "CREATE FUNCTION gv(lo int) RETURNS SETOF tt LANGUAGE sql VOLATILE AS 'SELECT * FROM tt WHERE a >= lo ORDER BY a';",
                "CREATE FUNCTION hv(lo int) RETURNS TABLE (x int, y text) LANGUAGE sql VOLATILE AS 'SELECT a, b FROM tt WHERE a >= lo ORDER BY a';",
                "CREATE FUNCTION ov(lo int, OUT x int, OUT y text) RETURNS SETOF record LANGUAGE sql AS 'SELECT a, b FROM tt WHERE a >= lo ORDER BY a';",
                "CREATE FUNCTION one(lo int) RETURNS tt LANGUAGE sql AS 'SELECT * FROM tt WHERE a = lo';",
                "CREATE FUNCTION ins(v int) RETURNS SETOF int LANGUAGE sql AS 'INSERT INTO tt VALUES (v, ''w'') RETURNING a';",
                "CREATE FUNCTION f(n int) RETURNS SETOF int LANGUAGE sql STABLE AS 'SELECT generate_series(1, n)';",
                "CREATE FUNCTION g(lo int) RETURNS SETOF tt LANGUAGE sql STABLE AS 'SELECT * FROM tt WHERE a >= lo';",
                "CREATE FUNCTION h(lo int) RETURNS TABLE (x int, y text) LANGUAGE sql STABLE AS 'SELECT a, b FROM tt WHERE a >= lo';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM fv(3);",
                    expected: Expected::Rows {
                        columns: &[Column("fv", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fv(3);",
                    expected: Expected::Rows {
                        columns: &[Column("fv", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fv(3), fv(2);",
                    expected: Expected::Rows {
                        columns: &[Column("fv", INT4), Column("fv", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM gv(2);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT gv(2);",
                    expected: Expected::Rows {
                        columns: &[Column("gv", USER_DEFINED)],
                        rows: &[
                            &[T("(2,y)")],
                            &[T("(3,z)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (gv(2)).b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("y")],
                            &[T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM hv(2);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hv(1);",
                    expected: Expected::Rows {
                        columns: &[Column("hv", RECORD)],
                        rows: &[
                            &[T("(1,x)")],
                            &[T("(2,y)")],
                            &[T("(3,z)")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ov(2);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ov(3);",
                    expected: Expected::Rows {
                        columns: &[Column("ov", RECORD)],
                        rows: &[
                            &[T("(3,z)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT one(2), (one(3)).b;",
                    expected: Expected::Rows {
                        columns: &[Column("one", USER_DEFINED), Column("b", TEXT)],
                        rows: &[
                            &[T("(2,y)"), T("z")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one(1);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fv(5) LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("fv", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fv(2) WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("fv", INT4), Column("ordinality", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ins(7);",
                    expected: Expected::Rows {
                        columns: &[Column("ins", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ins(8);",
                    expected: Expected::Rows {
                        columns: &[Column("ins", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM tt;",
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
                    query: "SELECT * FROM unnest(ARRAY[1,2]) u, LATERAL fv(u) f;",
                    expected: Expected::Rows {
                        columns: &[Column("u", INT4), Column("f", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM fv(3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on fv")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT fv(3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("ProjectSet")],
                            &[T("  ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM _pg_expandarray(ARRAY['a','b']);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function _pg_expandarray(text[]) does not exist", position: 15, hint: "No function matches the given name and argument types. You might need to add explicit type casts.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (information_schema._pg_expandarray(ARRAY['a','b'])).n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM f(3);",
                    expected: Expected::Rows {
                        columns: &[Column("f", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM g(2);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[T("7"), T("w")],
                            &[T("8"), T("w")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM h(2);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[T("7"), T("w")],
                            &[T("8"), T("w")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x FROM h(1) WHERE y > 'x';",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM g(2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on tt")],
                            &[T("  Filter: (a >= 2)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM f(3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("ProjectSet")],
                            &[T("  Output: generate_series(1, 3)")],
                            &[T("  ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM tt, LATERAL g(tt.a) s;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop")],
                            &[T("  Join Filter: (tt_1.a >= tt.a)")],
                            &[T("  ->  Seq Scan on tt")],
                            &[T("  ->  Materialize")],
                            &[T("        ->  Seq Scan on tt tt_1")],
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
fn test_sql_functions_returning_sets_lazily() {
    run_scripts(&[
        ScriptTest {
            name: "SQL functions returning sets a value per call: snapshots, early ends, and nesting",
            set_up_script: &[
                "CREATE TABLE src (a int);",
                "INSERT INTO src SELECT generate_series(1, 5);",
                "CREATE TABLE log (v int);",
                "CREATE SEQUENCE sq;",
                "CREATE FUNCTION rd() RETURNS SETOF int LANGUAGE sql STABLE AS 'SELECT a FROM src ORDER BY a';",
                "CREATE FUNCTION vol() RETURNS SETOF bigint LANGUAGE sql VOLATILE AS 'INSERT INTO log VALUES (0); SELECT count(*) FROM log';",
                "CREATE FUNCTION nx() RETURNS SETOF bigint LANGUAGE sql VOLATILE AS 'SELECT nextval(''sq'') FROM generate_series(1, 10)';",
                "CREATE FUNCTION outer_f(n int) RETURNS SETOF int LANGUAGE sql AS 'SELECT rd() * n';",
                "CREATE FUNCTION twice() RETURNS SETOF int LANGUAGE sql AS 'SELECT x FROM (SELECT rd() AS x) s WHERE x < 3';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT rd(), rd();",
                    expected: Expected::Rows {
                        columns: &[Column("rd", INT4), Column("rd", INT4)],
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
                    query: "SELECT outer_f(10);",
                    expected: Expected::Rows {
                        columns: &[Column("outer_f", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                            &[T("30")],
                            &[T("40")],
                            &[T("50")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT outer_f(rd());",
                    expected: Expected::Rows {
                        columns: &[Column("outer_f", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("2")],
                            &[T("4")],
                            &[T("6")],
                            &[T("8")],
                            &[T("10")],
                            &[T("3")],
                            &[T("6")],
                            &[T("9")],
                            &[T("12")],
                            &[T("15")],
                            &[T("4")],
                            &[T("8")],
                            &[T("12")],
                            &[T("16")],
                            &[T("20")],
                            &[T("5")],
                            &[T("10")],
                            &[T("15")],
                            &[T("20")],
                            &[T("25")],
                        ],
                        tag: "SELECT 25",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT twice(), twice();",
                    expected: Expected::Rows {
                        columns: &[Column("twice", INT4), Column("twice", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO log SELECT rd();",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM log;",
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
                    query: "SELECT vol();",
                    expected: Expected::Rows {
                        columns: &[Column("vol", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT vol();",
                    expected: Expected::Rows {
                        columns: &[Column("vol", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO src SELECT rd() + 100;",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM src;",
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
                    query: "SELECT nx() LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("nx", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT currval('sq');",
                    expected: Expected::Rows {
                        columns: &[Column("currval", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, rd() FROM src WHERE a < 3 ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("rd", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                            &[T("1"), T("3")],
                            &[T("1"), T("4")],
                            &[T("1"), T("5")],
                            &[T("1"), T("101")],
                            &[T("1"), T("102")],
                            &[T("1"), T("103")],
                            &[T("1"), T("104")],
                            &[T("1"), T("105")],
                            &[T("2"), T("1")],
                            &[T("2"), T("2")],
                            &[T("2"), T("3")],
                            &[T("2"), T("4")],
                            &[T("2"), T("5")],
                            &[T("2"), T("101")],
                            &[T("2"), T("102")],
                            &[T("2"), T("103")],
                            &[T("2"), T("104")],
                            &[T("2"), T("105")],
                        ],
                        tag: "SELECT 20",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nx() LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("nx", INT8)],
                        rows: &[
                            &[T("4")],
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
                    query: "ALTER TABLE src ADD COLUMN z int;",
                    client: "B",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT CASE WHEN g = 3 THEN 1 / 0 ELSE g END FROM (SELECT rd() AS g) s;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE src DROP COLUMN z;",
                    client: "B",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT rd();",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("rd", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE c;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE src ADD COLUMN y int;",
                    client: "B",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rd() LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("rd", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
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
fn test_sql_functions_returning_records() {
    run_scripts(&[
        ScriptTest {
            name: "SQL functions returning records, with column definition lists",
            set_up_script: &[
                "CREATE TABLE rt (a int, b int, c text);",
                "INSERT INTO rt VALUES (1, 1, 'x'), (1, 2, 'y'), (2, 1, 'z');",
                "CREATE FUNCTION rs(int) RETURNS SETOF record LANGUAGE sql AS 'SELECT * FROM rt WHERE a = $1';",
                "CREATE FUNCTION rsv(int) RETURNS SETOF record LANGUAGE sql VOLATILE AS 'SELECT * FROM rt WHERE a = $1 ORDER BY b';",
                "CREATE FUNCTION r1(int) RETURNS record LANGUAGE sql AS 'SELECT * FROM rt WHERE a = $1 ORDER BY b';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM rs(1) AS t(a int, b int, c text) ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("x")],
                            &[T("1"), T("2"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rsv(1) AS t(a int, b int, c text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("x")],
                            &[T("1"), T("2"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ROWS FROM (rsv(1) AS (a int, b int, c text)) WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("ordinality", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("x"), T("1")],
                            &[T("1"), T("2"), T("y"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ROWS FROM (rsv(1) AS (a int, b int, c text), generate_series(1, 3)) AS t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("generate_series", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("x"), T("1")],
                            &[T("1"), T("2"), T("y"), T("2")],
                            &[Null, Null, Null, T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r1(1) AS t(a int, b int, c text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t.c FROM rsv(2) AS t(a int, b int, c text);",
                    expected: Expected::Rows {
                        columns: &[Column("c", TEXT)],
                        rows: &[
                            &[T("z")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rs(1) AS t(a int, b int);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "return type mismatch in function declared to return record", detail: "Final statement returns too many columns.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rs(1) AS t(a int, b text, c text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("x")],
                            &[T("1"), T("2"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rs(1);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "a column definition list is required for functions returning \"record\"", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rsv(1);",
                    expected: Expected::Rows {
                        columns: &[Column("rsv", RECORD)],
                        rows: &[
                            &[T("(1,1,x)")],
                            &[T("(1,2,y)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r1(2);",
                    expected: Expected::Rows {
                        columns: &[Column("r1", RECORD)],
                        rows: &[
                            &[T("(2,1,z)")],
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
fn test_plpgsql_functions_returning_sets() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL functions returning sets with RETURN NEXT, RETURN QUERY, and RETURN QUERY EXECUTE",
            set_up_script: &[
                "CREATE TABLE pt (a int, b text);",
                "INSERT INTO pt VALUES (1, 'x'), (2, 'y'), (3, 'z');",
                "CREATE TABLE pt_log (n int, found bool);",
                "CREATE TYPE pair AS (a int, b text);",
                "CREATE FUNCTION nums(n int) RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN FOR i IN 1..n LOOP RETURN NEXT i * 10; END LOOP; END $$;",
                "CREATE FUNCTION tab(lo int) RETURNS TABLE (x int, y text) LANGUAGE plpgsql AS $$ BEGIN FOR x, y IN SELECT a, b FROM pt WHERE a >= lo ORDER BY a LOOP RETURN NEXT; END LOOP; END $$;",
                "CREATE FUNCTION pairs() RETURNS SETOF pair LANGUAGE plpgsql AS $$ DECLARE r pair; BEGIN FOR r IN SELECT a, b FROM pt ORDER BY a LOOP RETURN NEXT r; END LOOP; RETURN NEXT ROW(9, 'nine')::pair; RETURN NEXT NULL; END $$;",
                "CREATE FUNCTION q(lo int) RETURNS SETOF pt LANGUAGE plpgsql AS $$ DECLARE n int; BEGIN RETURN QUERY SELECT * FROM pt WHERE a >= lo ORDER BY a; GET DIAGNOSTICS n = ROW_COUNT; INSERT INTO pt_log VALUES (n, FOUND); RETURN QUERY SELECT a + 100, b FROM pt WHERE a < lo ORDER BY a; INSERT INTO pt_log VALUES (-1, FOUND); END $$;",
                "CREATE FUNCTION qe(t text, lo int) RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY EXECUTE format('SELECT a FROM %I WHERE a >= $1 ORDER BY a', t) USING lo; END $$;",
                "CREATE FUNCTION recs() RETURNS SETOF record LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT a, b FROM pt ORDER BY a LOOP RETURN NEXT r; END LOOP; END $$;",
                "CREATE FUNCTION early(n int) RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN RETURN NEXT 1; IF n > 0 THEN RETURN; END IF; RETURN NEXT 2; END $$;",
                "CREATE FUNCTION nothing() RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN END $$;",
                "CREATE FUNCTION ins(v int) RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY INSERT INTO pt VALUES (v, 'new') RETURNING a; END $$;",
                "CREATE FUNCTION exc() RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN RETURN NEXT 1; BEGIN RETURN NEXT 2; PERFORM 1 / 0; EXCEPTION WHEN division_by_zero THEN RETURN NEXT 3; END; RETURN NEXT 4; END $$;",
                "CREATE FUNCTION casts() RETURNS SETOF numeric LANGUAGE plpgsql AS $$ BEGIN RETURN NEXT 1; RETURN NEXT 2.5::float8; RETURN NEXT '3'; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM nums(3);",
                    expected: Expected::Rows {
                        columns: &[Column("nums", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                            &[T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nums(2), nums(3);",
                    expected: Expected::Rows {
                        columns: &[Column("nums", INT4), Column("nums", INT4)],
                        rows: &[
                            &[T("10"), T("10")],
                            &[T("20"), T("20")],
                            &[Null, T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM nums(3) WITH ORDINALITY AS t(v, n);",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("n", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                            &[T("20"), T("2")],
                            &[T("30"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tab(2);",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tab(3);",
                    expected: Expected::Rows {
                        columns: &[Column("tab", RECORD)],
                        rows: &[
                            &[T("(3,z)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pairs();",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[T("9"), T("nine")],
                            &[Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pairs()).b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[T("x")],
                            &[T("y")],
                            &[T("z")],
                            &[T("nine")],
                            &[Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q(2);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[T("101"), T("x")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("found", BOOL)],
                        rows: &[
                            &[T("2"), T("t")],
                            &[T("-1"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM qe('pt', 2);",
                    expected: Expected::Rows {
                        columns: &[Column("qe", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM recs() AS t(a int, b text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM early(1);",
                    expected: Expected::Rows {
                        columns: &[Column("early", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM early(0);",
                    expected: Expected::Rows {
                        columns: &[Column("early", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM nothing();",
                    expected: Expected::Rows {
                        columns: &[Column("nothing", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM nothing();",
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
                    query: "SELECT * FROM ins(4);",
                    expected: Expected::Rows {
                        columns: &[Column("ins", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM exc();",
                    expected: Expected::Rows {
                        columns: &[Column("exc", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM casts();",
                    expected: Expected::Rows {
                        columns: &[Column("casts", NUMERIC)],
                        rows: &[
                            &[T("1")],
                            &[T("2.5")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt, LATERAL nums(pt.a) n WHERE pt.a < 3 ORDER BY 1, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("n", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("10")],
                            &[T("2"), T("y"), T("10")],
                            &[T("2"), T("y"), T("20")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM recs();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "a column definition list is required for functions returning \"record\"", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM qe('pt', 3) WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("qe", INT4), Column("ordinality", INT8)],
                        rows: &[
                            &[T("3"), T("1")],
                            &[T("4"), T("2")],
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
fn test_plpgsql_functions_returning_sets_errors() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL functions returning sets: rows that do not match, statements without rows, and sets nobody takes",
            set_up_script: &[
                "CREATE TABLE pt (a int, b text);",
                "INSERT INTO pt VALUES (1, 'x'), (2, 'y');",
                "CREATE TYPE pair AS (a int, b text);",
                "CREATE FUNCTION qtype() RETURNS SETOF numeric LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT 7; END $$;",
                "CREATE FUNCTION qcount() RETURNS SETOF pt LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT a FROM pt; END $$;",
                "CREATE FUNCTION qins() RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY INSERT INTO pt VALUES (5, 'five'); END $$;",
                "CREATE FUNCTION qnull() RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY EXECUTE NULL; END $$;",
                "CREATE FUNCTION nrec() RETURNS SETOF pair LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN SELECT 1 AS a, 2 AS b, 3 AS c INTO r; RETURN NEXT r; END $$;",
                "CREATE FUNCTION nscalar() RETURNS SETOF pair LANGUAGE plpgsql AS $$ BEGIN RETURN NEXT 1; END $$;",
                "CREATE FUNCTION recs() RETURNS SETOF record LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT a, b FROM pt ORDER BY a LOOP RETURN NEXT r; END LOOP; END $$;",
                "CREATE FUNCTION qrecs() RETURNS SETOF record LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT b::varchar(5) FROM pt; END $$;",
                "CREATE FUNCTION unassigned() RETURNS SETOF pair LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN RETURN NEXT r; END $$;",
                "CREATE FUNCTION typed_null() RETURNS SETOF pair LANGUAGE plpgsql AS $$ DECLARE r pair; BEGIN RETURN NEXT r; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM qtype();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "structure of query does not match function result type", detail: "Returned type integer does not match expected type numeric in column \"\" (position 1).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM qcount();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "structure of query does not match function result type", detail: "Number of returned columns (1) does not match expected column count (2).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM qins();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT query does not return tuples", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pt;",
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
                    query: "SELECT * FROM qnull();",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "query string argument of EXECUTE is null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM nrec();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "wrong record type supplied in RETURN NEXT", detail: "Returned type integer does not match expected type text in column \"b\" (position 2).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM nscalar();",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "cannot return non-composite value from function returning composite type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT recs();",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "materialize mode required, but it is not allowed in this context", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM recs() AS t(a int, b int);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "wrong record type supplied in RETURN NEXT", detail: "Returned type text does not match expected type integer in column \"b\" (position 2).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM qrecs() AS t(b varchar(3));",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "structure of query does not match function result type", detail: "Returned type character varying(5) does not match expected type character varying(3) in column \"b\" (position 1).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM qrecs() AS t(b varchar);",
                    expected: Expected::Rows {
                        columns: &[Column("b", VARCHAR)],
                        rows: &[
                            &[T("x")],
                            &[T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unassigned();",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "record \"r\" is not assigned yet", detail: "The tuple structure of a not-yet-assigned record is indeterminate.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM typed_null();",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN NEXT 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "cannot use RETURN NEXT in a non-SETOF function", position: 64, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT 1; END $$;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "cannot use RETURN QUERY in a non-SETOF function", position: 64, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_plpgsql_functions_returning_records() {
    run_scripts(&[
        ScriptTest {
            name: "PL/pgSQL functions returning records, with column definition lists",
            set_up_script: &[
                "CREATE FUNCTION prec() RETURNS record LANGUAGE plpgsql AS $$ BEGIN RETURN ROW(1, 'x'::text); END $$;",
                "CREATE FUNCTION pout(OUT a int, OUT b text) LANGUAGE plpgsql AS $$ BEGIN a := 1; b := 'x'; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM prec() AS t(a int, b text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prec() AS t(a int, b varchar);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "returned record type does not match expected record type", detail: "Returned type text does not match expected type character varying in column \"b\" (position 2).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prec() AS t(a int);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "returned record type does not match expected record type", detail: "Number of returned columns (2) does not match expected column count (1).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prec() AS t(a bigint, b text);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "returned record type does not match expected record type", detail: "Returned type integer does not match expected type bigint in column \"a\" (position 1).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pout();",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT prec();",
                    expected: Expected::Rows {
                        columns: &[Column("prec", RECORD)],
                        rows: &[
                            &[T("(1,x)")],
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
fn test_partition_tree_functions_and_current_schemas() {
    run_scripts(&[
        ScriptTest {
            name: "pg_partition_tree, pg_partition_root, pg_partition_ancestors, and current_schemas",
            set_up_script: &[
                "CREATE TABLE ptif_test (a int, b int) PARTITION BY RANGE (a);",
                "CREATE TABLE ptif_test0 PARTITION OF ptif_test FOR VALUES FROM (MINVALUE) TO (0) PARTITION BY LIST (b);",
                "CREATE TABLE ptif_test01 PARTITION OF ptif_test0 FOR VALUES IN (1);",
                "CREATE TABLE ptif_test1 PARTITION OF ptif_test FOR VALUES FROM (0) TO (100) PARTITION BY LIST (b);",
                "CREATE TABLE ptif_test11 PARTITION OF ptif_test1 FOR VALUES IN (1);",
                "CREATE TABLE ptif_test2 PARTITION OF ptif_test FOR VALUES FROM (100) TO (200);",
                "CREATE TABLE ptif_test3 PARTITION OF ptif_test FOR VALUES FROM (200) TO (MAXVALUE) PARTITION BY LIST (b);",
                "CREATE INDEX ptif_test_index ON ONLY ptif_test (a);",
                "CREATE INDEX ptif_test0_index ON ONLY ptif_test0 (a);",
                "ALTER INDEX ptif_test_index ATTACH PARTITION ptif_test0_index;",
                "CREATE TABLE ptif_normal_table (a int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relid, parentrelid, level, isleaf FROM pg_partition_tree('ptif_test') p JOIN pg_class c ON (p.relid = c.oid) ORDER BY level, relid::text;",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS), Column("parentrelid", REGCLASS), Column("level", INT4), Column("isleaf", BOOL)],
                        rows: &[
                            &[T("ptif_test"), Null, T("0"), T("f")],
                            &[T("ptif_test0"), T("ptif_test"), T("1"), T("f")],
                            &[T("ptif_test1"), T("ptif_test"), T("1"), T("f")],
                            &[T("ptif_test2"), T("ptif_test"), T("1"), T("t")],
                            &[T("ptif_test3"), T("ptif_test"), T("1"), T("f")],
                            &[T("ptif_test01"), T("ptif_test0"), T("2"), T("t")],
                            &[T("ptif_test11"), T("ptif_test1"), T("2"), T("t")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relid, parentrelid, level, isleaf FROM pg_partition_tree('ptif_test0') p JOIN pg_class c ON (p.relid = c.oid) ORDER BY level, relid::text;",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS), Column("parentrelid", REGCLASS), Column("level", INT4), Column("isleaf", BOOL)],
                        rows: &[
                            &[T("ptif_test0"), T("ptif_test"), T("0"), T("f")],
                            &[T("ptif_test01"), T("ptif_test0"), T("1"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relid, parentrelid, level, isleaf FROM pg_partition_tree('ptif_test01') p JOIN pg_class c ON (p.relid = c.oid) ORDER BY level, relid::text;",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS), Column("parentrelid", REGCLASS), Column("level", INT4), Column("isleaf", BOOL)],
                        rows: &[
                            &[T("ptif_test01"), T("ptif_test0"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relid, parentrelid, level, isleaf FROM pg_partition_tree('ptif_test3') p JOIN pg_class c ON (p.relid = c.oid) ORDER BY level, relid::text;",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS), Column("parentrelid", REGCLASS), Column("level", INT4), Column("isleaf", BOOL)],
                        rows: &[
                            &[T("ptif_test3"), T("ptif_test"), T("0"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_partition_root('ptif_test'), pg_partition_root('ptif_test0'), pg_partition_root('ptif_test01'), pg_partition_root('ptif_test3');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_partition_root", REGCLASS), Column("pg_partition_root", REGCLASS), Column("pg_partition_root", REGCLASS), Column("pg_partition_root", REGCLASS)],
                        rows: &[
                            &[T("ptif_test"), T("ptif_test"), T("ptif_test"), T("ptif_test")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relid, parentrelid, level, isleaf FROM pg_partition_tree('ptif_test_index') p JOIN pg_class c ON (p.relid = c.oid) ORDER BY level, relid::text;",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS), Column("parentrelid", REGCLASS), Column("level", INT4), Column("isleaf", BOOL)],
                        rows: &[
                            &[T("ptif_test_index"), Null, T("0"), T("f")],
                            &[T("ptif_test0_index"), T("ptif_test_index"), T("1"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_partition_root('ptif_test0_index');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_partition_root", REGCLASS)],
                        rows: &[
                            &[T("ptif_test_index")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_partition_ancestors('ptif_test01');",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS)],
                        rows: &[
                            &[T("ptif_test01")],
                            &[T("ptif_test0")],
                            &[T("ptif_test")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_partition_ancestors('ptif_test0_index');",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS)],
                        rows: &[
                            &[T("ptif_test0_index")],
                            &[T("ptif_test_index")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_partition_tree('ptif_normal_table');",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS), Column("parentrelid", REGCLASS), Column("isleaf", BOOL), Column("level", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_partition_root('ptif_normal_table');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_partition_root", REGCLASS)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_partition_ancestors('ptif_normal_table');",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_partition_tree(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS), Column("parentrelid", REGCLASS), Column("isleaf", BOOL), Column("level", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_partition_root(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_partition_root", REGCLASS)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_partition_tree(0);",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS), Column("parentrelid", REGCLASS), Column("isleaf", BOOL), Column("level", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_partition_root(0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_partition_root", REGCLASS)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_partition_tree('ptif_test1')::text;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_partition_tree", TEXT)],
                        rows: &[
                            &[T("(ptif_test1,ptif_test,f,0)")],
                            &[T("(ptif_test11,ptif_test1,t,1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_partition_ancestors('ptif_test11'), pg_partition_ancestors('ptif_test1');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_partition_ancestors", REGCLASS), Column("pg_partition_ancestors", REGCLASS)],
                        rows: &[
                            &[T("ptif_test11"), T("ptif_test1")],
                            &[T("ptif_test1"), T("ptif_test")],
                            &[T("ptif_test"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false), current_schemas(true);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY), Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{public}"), T("{pg_catalog,public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = nosuch, public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false), current_schemas(true);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY), Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{public}"), T("{pg_catalog,public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = nosuch;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false), current_schemas(true), current_schema();",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY), Column("current_schemas", NAME_ARRAY), Column("current_schema", NAME)],
                        rows: &[
                            &[T("{}"), T("{pg_catalog}"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = public, pg_catalog;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false), current_schemas(true);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY), Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{public,pg_catalog}"), T("{public,pg_catalog}")],
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
fn test_keywords_and_options_to_table() {
    run_scripts(&[
        ScriptTest {
            name: "pg_get_keywords and pg_options_to_table",
            set_up_script: &[
                "CREATE TABLE opt_t (a int) WITH (fillfactor = 70, autovacuum_enabled = false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), count(*) FILTER (WHERE barelabel), count(*) FILTER (WHERE catcode = 'R') FROM pg_get_keywords();",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("494"), T("455"), T("78")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_get_keywords() WHERE word IN ('abort', 'all', 'between', 'binary', 'from', 'year', 'zone') ORDER BY word;",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("catcode", CHAR), Column("barelabel", BOOL), Column("catdesc", TEXT), Column("baredesc", TEXT)],
                        rows: &[
                            &[T("abort"), T("U"), T("t"), T("unreserved"), T("can be bare label")],
                            &[T("all"), T("R"), T("t"), T("reserved"), T("can be bare label")],
                            &[T("between"), T("C"), T("t"), T("unreserved (cannot be function or type name)"), T("can be bare label")],
                            &[T("binary"), T("T"), T("t"), T("reserved (can be function or type name)"), T("can be bare label")],
                            &[T("from"), T("R"), T("f"), T("reserved"), T("requires AS")],
                            &[T("year"), T("U"), T("f"), T("unreserved"), T("requires AS")],
                            &[T("zone"), T("U"), T("t"), T("unreserved"), T("can be bare label")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT catcode, catdesc, barelabel, baredesc, count(*) FROM pg_get_keywords() GROUP BY 1, 2, 3, 4 ORDER BY 1, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("catcode", CHAR), Column("catdesc", TEXT), Column("barelabel", BOOL), Column("baredesc", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("C"), T("unreserved (cannot be function or type name)"), T("f"), T("requires AS"), T("3")],
                            &[T("C"), T("unreserved (cannot be function or type name)"), T("t"), T("can be bare label"), T("60")],
                            &[T("R"), T("reserved"), T("f"), T("requires AS"), T("22")],
                            &[T("R"), T("reserved"), T("t"), T("can be bare label"), T("56")],
                            &[T("T"), T("reserved (can be function or type name)"), T("f"), T("requires AS"), T("3")],
                            &[T("T"), T("reserved (can be function or type name)"), T("t"), T("can be bare label"), T("20")],
                            &[T("U"), T("unreserved"), T("f"), T("requires AS"), T("11")],
                            &[T("U"), T("unreserved"), T("t"), T("can be bare label"), T("319")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT word FROM pg_get_keywords() ORDER BY word LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT)],
                        rows: &[
                            &[T("abort")],
                            &[T("absent")],
                            &[T("absolute")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_get_keywords()).word LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT)],
                        rows: &[
                            &[T("abort")],
                            &[T("absent")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_options_to_table(ARRAY['fillfactor=70', 'autovacuum_enabled=false', 'novalue', 'a=b=c', '=x']);",
                    expected: Expected::Rows {
                        columns: &[Column("option_name", TEXT), Column("option_value", TEXT)],
                        rows: &[
                            &[T("fillfactor"), T("70")],
                            &[T("autovacuum_enabled"), T("false")],
                            &[T("novalue"), Null],
                            &[T("a"), T("b=c")],
                            &[T(""), T("x")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_options_to_table((SELECT reloptions FROM pg_class WHERE relname = 'opt_t'));",
                    expected: Expected::Rows {
                        columns: &[Column("option_name", TEXT), Column("option_value", TEXT)],
                        rows: &[
                            &[T("fillfactor"), T("70")],
                            &[T("autovacuum_enabled"), T("false")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_options_to_table('{}');",
                    expected: Expected::Rows {
                        columns: &[Column("option_name", TEXT), Column("option_value", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_options_to_table(NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("option_name", TEXT), Column("option_value", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_options_to_table(ARRAY['a=1', NULL]);",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "null array element not allowed in this context", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT option_name, option_value FROM pg_class, pg_options_to_table(reloptions) WHERE relname = 'opt_t' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("option_name", TEXT), Column("option_value", TEXT)],
                        rows: &[
                            &[T("autovacuum_enabled"), T("false")],
                            &[T("fillfactor"), T("70")],
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
fn test_json_set_returning_functions() {
    run_scripts(&[
        ScriptTest {
            name: "json and jsonb object keys, each, array elements, and records from arrays of objects",
            set_up_script: &[
                "CREATE TYPE jpop AS (a text, b int, c timestamp);",
                "CREATE TYPE jpop2 AS (a int, b json, c int, d int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM json_object_keys('{\"a\":1,\"b\":{\"c\":2,\"d\":3},\"e\":[4]}');",
                    expected: Expected::Rows {
                        columns: &[Column("json_object_keys", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("e")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_object_keys('{\"b\":1,\"a\":{\"c\":2},\"aa\":[4]}');",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_object_keys", TEXT)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("aa")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT json_object_keys('{\"x\":1}'), jsonb_object_keys('{\"y\":2,\"z\":3}');",
                    expected: Expected::Rows {
                        columns: &[Column("json_object_keys", TEXT), Column("jsonb_object_keys", TEXT)],
                        rows: &[
                            &[T("x"), T("y")],
                            &[Null, T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_object_keys('[1,2]');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call json_object_keys on an array", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_object_keys('3');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call json_object_keys on a scalar", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_object_keys('[1,2]');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call jsonb_object_keys on an array", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_object_keys('\"s\"');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call jsonb_object_keys on a scalar", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_each('{\"f1\":[1,2,3],\"f2\":{\"f3\":1},\"f4\":null,\"f5\":99,\"f6\":\"stringy\"}') q;",
                    expected: Expected::Rows {
                        columns: &[Column("key", TEXT), Column("value", JSON)],
                        rows: &[
                            &[T("f1"), T("[1,2,3]")],
                            &[T("f2"), T("{\"f3\":1}")],
                            &[T("f4"), T("null")],
                            &[T("f5"), T("99")],
                            &[T("f6"), T("\"stringy\"")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_each_text('{\"f1\":[1,2,3],\"f2\":{\"f3\":1},\"f4\":null,\"f5\":99,\"f6\":\"stringy\",\"f7\":\"a\\\"b\"}') q;",
                    expected: Expected::Rows {
                        columns: &[Column("key", TEXT), Column("value", TEXT)],
                        rows: &[
                            &[T("f1"), T("[1,2,3]")],
                            &[T("f2"), T("{\"f3\":1}")],
                            &[T("f4"), Null],
                            &[T("f5"), T("99")],
                            &[T("f6"), T("stringy")],
                            &[T("f7"), T("a\"b")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_each('{\"f1\":[1,2,3],\"f2\":{\"f3\":1},\"f4\":null,\"f5\":99,\"f6\":\"stringy\"}') q;",
                    expected: Expected::Rows {
                        columns: &[Column("key", TEXT), Column("value", JSONB)],
                        rows: &[
                            &[T("f1"), T("[1, 2, 3]")],
                            &[T("f2"), T("{\"f3\": 1}")],
                            &[T("f4"), T("null")],
                            &[T("f5"), T("99")],
                            &[T("f6"), T("\"stringy\"")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_each_text('{\"f1\":[1,2,3],\"f2\":{\"f3\":1},\"f4\":null,\"f5\":99,\"f6\":\"stringy\"}') q;",
                    expected: Expected::Rows {
                        columns: &[Column("key", TEXT), Column("value", TEXT)],
                        rows: &[
                            &[T("f1"), T("[1, 2, 3]")],
                            &[T("f2"), T("{\"f3\": 1}")],
                            &[T("f4"), Null],
                            &[T("f5"), T("99")],
                            &[T("f6"), T("stringy")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_each('[1]');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot deconstruct an array as an object", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_each('1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot deconstruct a scalar", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_each('[1]');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call jsonb_each on a non-object", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_each_text('1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call jsonb_each_text on a non-object", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT json_each('{\"a\":1}')::text, (jsonb_each('{\"b\":true}')).value;",
                    expected: Expected::Rows {
                        columns: &[Column("json_each", TEXT), Column("value", JSONB)],
                        rows: &[
                            &[T("(a,1)"), T("true")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_array_elements('[1,true,[1,[2,3]],null,{\"f1\":1,\"f2\":[7,8,9]},false,\"stringy\"]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("value", JSON)],
                        rows: &[
                            &[T("1")],
                            &[T("true")],
                            &[T("[1,[2,3]]")],
                            &[T("null")],
                            &[T("{\"f1\":1,\"f2\":[7,8,9]}")],
                            &[T("false")],
                            &[T("\"stringy\"")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_array_elements_text('[1,true,[1,[2,3]],null,{\"f1\":1,\"f2\":[7,8,9]},false,\"stringy\",\"a\\\"b\"]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("value", TEXT)],
                        rows: &[
                            &[T("1")],
                            &[T("true")],
                            &[T("[1,[2,3]]")],
                            &[Null],
                            &[T("{\"f1\":1,\"f2\":[7,8,9]}")],
                            &[T("false")],
                            &[T("stringy")],
                            &[T("a\"b")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_array_elements('[1,true,[1,[2,3]],null,{\"f1\":1,\"f2\":[7,8,9]},false,\"stringy\"]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("value", JSONB)],
                        rows: &[
                            &[T("1")],
                            &[T("true")],
                            &[T("[1, [2, 3]]")],
                            &[T("null")],
                            &[T("{\"f1\": 1, \"f2\": [7, 8, 9]}")],
                            &[T("false")],
                            &[T("\"stringy\"")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_array_elements_text('[1,true,[1,[2,3]],null,{\"f1\":1,\"f2\":[7,8,9]},false,\"stringy\"]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("value", TEXT)],
                        rows: &[
                            &[T("1")],
                            &[T("true")],
                            &[T("[1, [2, 3]]")],
                            &[Null],
                            &[T("{\"f1\": 1, \"f2\": [7, 8, 9]}")],
                            &[T("false")],
                            &[T("stringy")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_array_elements('{\"a\":1}');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call json_array_elements on a non-array", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_array_elements_text('1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call json_array_elements_text on a scalar", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_array_elements('{\"a\":1}');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot extract elements from an object", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_array_elements_text('\"x\"');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot extract elements from a scalar", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_array_elements('[1,2]') WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("value", JSON), Column("ordinality", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(NULL::jpop, '[{\"a\":\"blurfl\",\"x\":43.2},{\"b\":3,\"c\":\"2012-01-20 10:42:53\"}]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", INT4), Column("c", TIMESTAMP)],
                        rows: &[
                            &[T("blurfl"), Null, Null],
                            &[Null, T("3"), T("2012-01-20 10:42:53")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(ROW('def',99,NULL)::jpop, '[{\"a\":\"blurfl\",\"x\":43.2},{\"b\":3,\"c\":\"2012-01-20 10:42:53\"}]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", INT4), Column("c", TIMESTAMP)],
                        rows: &[
                            &[T("blurfl"), T("99"), Null],
                            &[T("def"), T("3"), T("2012-01-20 10:42:53")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_populate_recordset(NULL::jpop, '[{\"a\":\"blurfl\",\"x\":43.2},{\"b\":3,\"c\":\"2012-01-20 10:42:53\"}]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", INT4), Column("c", TIMESTAMP)],
                        rows: &[
                            &[T("blurfl"), Null, Null],
                            &[Null, T("3"), T("2012-01-20 10:42:53")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_populate_recordset(ROW('def',99,NULL)::jpop, '[{\"a\":\"blurfl\",\"x\":43.2},{\"b\":3,\"c\":\"2012-01-20 10:42:53\"}]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", INT4), Column("c", TIMESTAMP)],
                        rows: &[
                            &[T("blurfl"), T("99"), Null],
                            &[T("def"), T("3"), T("2012-01-20 10:42:53")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(NULL::jpop, '[{\"a\":[100,200,false],\"x\":43.2},{\"a\":{\"z\":true},\"b\":3,\"c\":\"2012-01-20 10:42:53\"}]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", INT4), Column("c", TIMESTAMP)],
                        rows: &[
                            &[T("[100,200,false]"), Null, Null],
                            &[T("{\"z\":true}"), T("3"), T("2012-01-20 10:42:53")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(NULL::jpop2, '[{\"a\":1,\"b\":{\"x\":[1,2]},\"c\":3}]') q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", JSON), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("{\"x\":[1,2]}"), T("3"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_to_recordset('[{\"a\":1,\"b\":\"foo\",\"d\":false},{\"a\":2,\"b\":\"bar\",\"c\":true}]') AS x(a int, b text, c boolean);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", BOOL)],
                        rows: &[
                            &[T("1"), T("foo"), Null],
                            &[T("2"), T("bar"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_to_recordset('[{\"a\":1,\"b\":\"foo\",\"d\":false},{\"a\":2,\"b\":\"bar\",\"c\":true}]') AS x(a int, b text, c boolean);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", BOOL)],
                        rows: &[
                            &[T("1"), T("foo"), Null],
                            &[T("2"), T("bar"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_to_recordset('[{\"a\":1,\"b\":{\"d\":\"foo\"},\"c\":true},{\"a\":2,\"c\":false,\"b\":{\"d\":\"bar\"}}]') AS x(a int, b json, c boolean);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", JSON), Column("c", BOOL)],
                        rows: &[
                            &[T("1"), T("{\"d\":\"foo\"}"), T("t")],
                            &[T("2"), T("{\"d\":\"bar\"}"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_to_record('{\"a\":1,\"b\":\"foo\",\"c\":\"bar\"}') AS x(a int, b text, d text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("foo"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_to_record('{\"a\":1,\"b\":\"foo\",\"c\":\"bar\"}') AS x(a int, b text, d text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("foo"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_record(NULL::record, '{\"a\":1}') AS x(a int, b text);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT json_to_record('{\"a\":1}');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "could not determine row type for result of json_to_record", hint: "Provide a non-null record argument, or call the function in the FROM clause using a column definition list.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_to_recordset(NULL) AS x(a int);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(NULL::jpop, NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", INT4), Column("c", TIMESTAMP)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(NULL::jpop, '{\"a\":1}');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call json_populate_recordset on an object", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(NULL::jpop, '1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call json_populate_recordset on a scalar", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(NULL::jpop, '[1]');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "argument of json_populate_recordset must be an array of objects", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_populate_recordset(NULL::jpop, '{\"a\":1}');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot call jsonb_populate_recordset on a non-array", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_populate_recordset(NULL::jpop, '[1]');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "argument of jsonb_populate_recordset must be an array of objects", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(NULL::jpop, '[{\"b\":\"x\"}]');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"x\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_populate_recordset(1, '[]');",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "first argument of json_populate_recordset must be a row type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT json_populate_recordset(NULL::jpop, '[{\"a\":\"q\"}]');",
                    expected: Expected::Rows {
                        columns: &[Column("json_populate_recordset", USER_DEFINED)],
                        rows: &[
                            &[T("(q,,)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM json_to_recordset('[{\"a\":1}]');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "a column definition list is required for functions returning \"record\"", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_foreign_keys_path_queries_time_zones_and_mcv_items() {
    run_scripts(&[
        ScriptTest {
            name: "pg_get_catalog_foreign_keys, jsonb_path_query, pg_timezone_names, pg_timezone_abbrevs, and pg_mcv_list_items",
            set_up_script: &[
                "CREATE TABLE mcv_t (a int, b text) WITH (autovacuum_enabled = false);",
                "INSERT INTO mcv_t SELECT i % 3, CASE WHEN i % 3 = 2 THEN NULL ELSE 'v' || (i % 3) END FROM generate_series(1, 30) i;",
                "CREATE STATISTICS mcv_st (mcv) ON a, b FROM mcv_t;",
                "ANALYZE mcv_t;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), count(*) FILTER (WHERE is_array), count(*) FILTER (WHERE is_opt) FROM pg_get_catalog_foreign_keys();",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("219"), T("19"), T("83")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fktable, fkcols, pktable, pkcols, is_array, is_opt FROM pg_get_catalog_foreign_keys() WHERE fktable = 'pg_constraint'::regclass ORDER BY fkcols::text;",
                    expected: Expected::Rows {
                        columns: &[Column("fktable", REGCLASS), Column("fkcols", TEXT_ARRAY), Column("pktable", REGCLASS), Column("pkcols", TEXT_ARRAY), Column("is_array", BOOL), Column("is_opt", BOOL)],
                        rows: &[
                            &[T("pg_constraint"), T("{conexclop}"), T("pg_operator"), T("{oid}"), T("t"), T("f")],
                            &[T("pg_constraint"), T("{conffeqop}"), T("pg_operator"), T("{oid}"), T("t"), T("f")],
                            &[T("pg_constraint"), T("{confrelid,confkey}"), T("pg_attribute"), T("{attrelid,attnum}"), T("t"), T("f")],
                            &[T("pg_constraint"), T("{confrelid}"), T("pg_class"), T("{oid}"), T("f"), T("t")],
                            &[T("pg_constraint"), T("{conindid}"), T("pg_class"), T("{oid}"), T("f"), T("t")],
                            &[T("pg_constraint"), T("{connamespace}"), T("pg_namespace"), T("{oid}"), T("f"), T("f")],
                            &[T("pg_constraint"), T("{conparentid}"), T("pg_constraint"), T("{oid}"), T("f"), T("t")],
                            &[T("pg_constraint"), T("{conpfeqop}"), T("pg_operator"), T("{oid}"), T("t"), T("f")],
                            &[T("pg_constraint"), T("{conppeqop}"), T("pg_operator"), T("{oid}"), T("t"), T("f")],
                            &[T("pg_constraint"), T("{conrelid,conkey}"), T("pg_attribute"), T("{attrelid,attnum}"), T("t"), T("t")],
                            &[T("pg_constraint"), T("{conrelid}"), T("pg_class"), T("{oid}"), T("f"), T("t")],
                            &[T("pg_constraint"), T("{contypid}"), T("pg_type"), T("{oid}"), T("f"), T("t")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT fktable, fkcols, pktable, pkcols FROM pg_get_catalog_foreign_keys() WHERE array_length(fkcols, 1) > 1 ORDER BY 1::text, 2::text;",
                    expected: Expected::Rows {
                        columns: &[Column("fktable", REGCLASS), Column("fkcols", TEXT_ARRAY), Column("pktable", REGCLASS), Column("pkcols", TEXT_ARRAY)],
                        rows: &[
                            &[T("pg_attrdef"), T("{adrelid,adnum}"), T("pg_attribute"), T("{attrelid,attnum}")],
                            &[T("pg_constraint"), T("{conrelid,conkey}"), T("pg_attribute"), T("{attrelid,attnum}")],
                            &[T("pg_constraint"), T("{confrelid,confkey}"), T("pg_attribute"), T("{attrelid,attnum}")],
                            &[T("pg_index"), T("{indrelid,indkey}"), T("pg_attribute"), T("{attrelid,attnum}")],
                            &[T("pg_statistic"), T("{starelid,staattnum}"), T("pg_attribute"), T("{attrelid,attnum}")],
                            &[T("pg_statistic_ext"), T("{stxrelid,stxkeys}"), T("pg_attribute"), T("{attrelid,attnum}")],
                            &[T("pg_trigger"), T("{tgrelid,tgattr}"), T("pg_attribute"), T("{attrelid,attnum}")],
                            &[T("pg_partitioned_table"), T("{partrelid,partattrs}"), T("pg_attribute"), T("{attrelid,attnum}")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_path_query('{\"a\": [1, 2, {\"b\": 3}]}', '$.a[*]');",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query", JSONB)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("{\"b\": 3}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_path_query('{\"a\": [1, 2, 3, 4]}', '$.a[*] ? (@ > $min)', '{\"min\": 2}');",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query", JSONB)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT jsonb_path_query('[1, \"x\", null]', '$[*]'), 1;",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query", JSONB), Column("?column?", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("\"x\""), T("1")],
                            &[T("null"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_path_query('{\"a\": 1}', 'strict $.b');",
                    expected: Expected::Error(Diagnostic { code: "2203A", message: "JSON object does not contain key \"b\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_path_query('{\"a\": 1}', 'strict $.b', '{}', true);",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query", JSONB)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_path_query('[]', '$[*]');",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query", JSONB)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_path_query_tz('[\"2015-08-01 12:00:00 +05\"]', '$[*].datetime() ? (@ < \"2015-08-02\".datetime())');",
                    expected: Expected::Rows {
                        columns: &[Column("jsonb_path_query_tz", JSONB)],
                        rows: &[
                            &[T("\"2015-08-01T12:00:00+05:00\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jsonb_path_query('[\"2015-08-01 12:00:00 +05\"]', '$[*].datetime() ? (@ < \"2015-08-02\".datetime())');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot convert value from date to timestamptz without time zone usage", hint: "Use *_tz() function for time zone support.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, abbrev, utc_offset, is_dst FROM pg_timezone_names WHERE name IN ('UTC', 'Asia/Kolkata', 'Etc/GMT+5', 'Asia/Tokyo', 'Etc/GMT-14') ORDER BY name COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("abbrev", TEXT), Column("utc_offset", INTERVAL), Column("is_dst", BOOL)],
                        rows: &[
                            &[T("Asia/Kolkata"), T("IST"), T("05:30:00"), T("f")],
                            &[T("Asia/Tokyo"), T("JST"), T("09:00:00"), T("f")],
                            &[T("Etc/GMT+5"), T("-05"), T("-05:00:00"), T("f")],
                            &[T("Etc/GMT-14"), T("+14"), T("14:00:00"), T("f")],
                            &[T("UTC"), T("UTC"), T("00:00:00"), T("f")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) > 300 FROM pg_timezone_names;",
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
                    query: "SELECT abbrev, utc_offset, is_dst FROM pg_timezone_abbrevs WHERE abbrev IN ('EST', 'MSK', 'NZDT', 'UTC', 'JST', 'AWST') ORDER BY abbrev;",
                    expected: Expected::Rows {
                        columns: &[Column("abbrev", TEXT), Column("utc_offset", INTERVAL), Column("is_dst", BOOL)],
                        rows: &[
                            &[T("AWST"), T("08:00:00"), T("f")],
                            &[T("EST"), T("-05:00:00"), T("f")],
                            &[T("JST"), T("09:00:00"), T("f")],
                            &[T("MSK"), T("03:00:00"), T("f")],
                            &[T("NZDT"), T("13:00:00"), T("t")],
                            &[T("UTC"), T("00:00:00"), T("f")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT abbrev, utc_offset, is_dst FROM pg_timezone_abbrevs_abbrevs() WHERE abbrev IN ('EST', 'MSK', 'NZDT', 'UTC', 'JST', 'AWST') ORDER BY abbrev COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("abbrev", TEXT), Column("utc_offset", INTERVAL), Column("is_dst", BOOL)],
                        rows: &[
                            &[T("AWST"), T("08:00:00"), T("f")],
                            &[T("EST"), T("-05:00:00"), T("f")],
                            &[T("JST"), T("09:00:00"), T("f")],
                            &[T("MSK"), T("03:00:00"), T("f")],
                            &[T("NZDT"), T("13:00:00"), T("t")],
                            &[T("UTC"), T("00:00:00"), T("f")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET timezone = 'Asia/Tokyo';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_timezone_abbrevs_zone();",
                    expected: Expected::Rows {
                        columns: &[Column("abbrev", TEXT), Column("utc_offset", INTERVAL), Column("is_dst", BOOL)],
                        rows: &[
                            &[T("LMT"), T("09:18:59"), T("f")],
                            &[T("JDT"), T("10:00:00"), T("t")],
                            &[T("JST"), T("09:00:00"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET timezone = 'UTC';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_timezone_abbrevs_zone();",
                    expected: Expected::Rows {
                        columns: &[Column("abbrev", TEXT), Column("utc_offset", INTERVAL), Column("is_dst", BOOL)],
                        rows: &[
                            &[T("UTC"), T("00:00:00"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT m.* FROM pg_statistic_ext s, pg_statistic_ext_data d, pg_mcv_list_items(d.stxdmcv) m WHERE s.oid = d.stxoid AND s.stxname = 'mcv_st' ORDER BY m.index;",
                    expected: Expected::Rows {
                        columns: &[Column("index", INT4), Column("values", TEXT_ARRAY), Column("nulls", BOOL_ARRAY), Column("frequency", FLOAT8), Column("base_frequency", FLOAT8)],
                        rows: &[
                            &[T("0"), T("{0,v0}"), T("{f,f}"), T("0.3333333333333333"), T("0.1111111111111111")],
                            &[T("1"), T("{1,v1}"), T("{f,f}"), T("0.3333333333333333"), T("0.1111111111111111")],
                            &[T("2"), T("{2,NULL}"), T("{f,t}"), T("0.3333333333333333"), T("0.1111111111111111")],
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
fn test_set_returning_functions_of_types_to_come() {
    run_scripts(&[
        ScriptTest {
            name: "aclexplode, pg_snapshot_xip, txid_snapshot_xip, and unnest of tsvector",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT grantor = 'postgres'::regrole AS by_postgres, grantee = 0 AS to_public, privilege_type, is_grantable FROM aclexplode('{postgres=arwdDxtm/postgres,=r/postgres}'::aclitem[]) ORDER BY privilege_type COLLATE \"C\", 2;",
                    expected: Expected::Rows {
                        columns: &[Column("by_postgres", BOOL), Column("to_public", BOOL), Column("privilege_type", TEXT), Column("is_grantable", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("DELETE"), T("f")],
                            &[T("t"), T("f"), T("INSERT"), T("f")],
                            &[T("t"), T("f"), T("MAINTAIN"), T("f")],
                            &[T("t"), T("f"), T("REFERENCES"), T("f")],
                            &[T("t"), T("f"), T("SELECT"), T("f")],
                            &[T("t"), T("t"), T("SELECT"), T("f")],
                            &[T("t"), T("f"), T("TRIGGER"), T("f")],
                            &[T("t"), T("f"), T("TRUNCATE"), T("f")],
                            &[T("t"), T("f"), T("UPDATE"), T("f")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM aclexplode('{}'::aclitem[]);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "ACL arrays must be one-dimensional", ..E }),
                    skip: Some("aclexplode waits on aclitem, which comes with GRANT (WinDG 5, phase 7)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_snapshot_xip('10:20:10,14,15');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_snapshot_xip", XID8)],
                        rows: &[
                            &[T("10")],
                            &[T("14")],
                            &[T("15")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM txid_snapshot_xip('10:20:10,14,15');",
                    expected: Expected::Rows {
                        columns: &[Column("txid_snapshot_xip", INT8)],
                        rows: &[
                            &[T("10")],
                            &[T("14")],
                            &[T("15")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest('a:1 b:2,3'::tsvector);",
                    expected: Expected::Rows {
                        columns: &[Column("lexeme", TEXT), Column("positions", INT2_ARRAY), Column("weights", TEXT_ARRAY)],
                        rows: &[
                            &[T("a"), T("{1}"), T("{D}")],
                            &[T("b"), T("{2,3}"), T("{D,D}")],
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
fn test_function_scan_join_without_materialize() {
    run_scripts(&[
        ScriptTest {
            name: "a nested loop rescans a function scan's kept rows without a Materialize",
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM generate_series(1, 1000) a, generate_series(1, 10) b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop")],
                            &[T("  ->  Function Scan on generate_series b")],
                            &[T("  ->  Function Scan on generate_series a")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM generate_series(1, 1000) a, LATERAL (SELECT a.a FROM generate_series(1, 10)) b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop")],
                            &[T("  ->  Function Scan on generate_series")],
                            &[T("  ->  Function Scan on generate_series a")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a * b) FROM generate_series(1, 1000) a, generate_series(1, 10) b;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("10000"), T("27527500")],
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
fn test_constant_functions_in_from() {
    run_scripts(&[
        ScriptTest {
            name: "a FROM function reduced to a constant of a scalar type becomes that constant",
            set_up_script: &[
                "CREATE TABLE pc (y INT) WITH (autovacuum_enabled = false);",
                "INSERT INTO pc VALUES (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT x, y FROM abs(-5) x, pc ORDER BY y;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("5"), T("1")],
                            &[T("5"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT x, y FROM abs(-5) x, pc WHERE x > y;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on pc")],
                            &[T("  Filter: (5 > y)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM upper('a') u;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM upper('a') WITH ORDINALITY AS u(v, n);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on u")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x, count(*) FROM abs(3) x GROUP BY ROLLUP(x) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("3"), T("1")],
                            &[Null, T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM generate_series(1, 2) g;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on generate_series g")],
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
fn test_sets_returned_at_once_past_work_mem() {
    run_scripts(&[
        ScriptTest {
            name: "sets returned at once and transition tables spill past work_mem and read back whole",
            set_up_script: &[
                "CREATE FUNCTION next_rows(n INT) RETURNS SETOF INT LANGUAGE plpgsql AS $$ BEGIN FOR i IN 1..n LOOP RETURN NEXT i; END LOOP; END $$;",
                "CREATE FUNCTION query_rows(n INT) RETURNS TABLE (i INT, t TEXT) LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT g, repeat('x', 50) || g FROM generate_series(1, n) g; END $$;",
                "CREATE FUNCTION sql_rows(n INT) RETURNS TABLE (i INT, t TEXT) LANGUAGE sql AS $$ SELECT g, md5(g::text) FROM generate_series(1, n) g $$;",
                "CREATE FUNCTION sql_values(n INT) RETURNS SETOF TEXT LANGUAGE sql AS $$ SELECT md5(g::text) FROM generate_series(1, n) g $$;",
                "CREATE TABLE tt (i INT, t TEXT);",
                "CREATE TABLE tt_log (rows BIGINT, total BIGINT);",
                "CREATE FUNCTION tt_count() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN INSERT INTO tt_log SELECT count(*), sum(i) FROM new_rows; RETURN NULL; END $$;",
                "CREATE TRIGGER tt_ins AFTER INSERT ON tt REFERENCING NEW TABLE AS new_rows FOR EACH STATEMENT EXECUTE FUNCTION tt_count();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(x) FROM next_rows(50000) x;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("50000"), T("1250025000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(i), max(t) FROM query_rows(20000);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("max", TEXT)],
                        rows: &[
                            &[T("20000"), T("200010000"), T("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx9999")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(i), min(t) FROM sql_rows(20000);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("min", TEXT)],
                        rows: &[
                            &[T("20000"), T("200010000"), T("00003e3b9e5336685200ae85d21b4f5e")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), min(v) FROM sql_values(20000) v;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("min", TEXT)],
                        rows: &[
                            &[T("20000"), T("00003e3b9e5336685200ae85d21b4f5e")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(x) FROM (SELECT next_rows(30000) AS x) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("30000"), T("450015000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(DISTINCT key) FROM jsonb_each((SELECT jsonb_object_agg('k' || g, g) FROM generate_series(1, 20000) g));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("20000"), T("20000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt SELECT g, repeat('y', 100) FROM generate_series(1, 20000) g;",
                    expected: Expected::Tag("INSERT 0 20000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("rows", INT8), Column("total", INT8)],
                        rows: &[
                            &[T("20000"), T("200010000")],
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
                    query: "DECLARE c SCROLL CURSOR FOR SELECT * FROM sql_rows(5000);",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("5000"), T("a35fe7f7fe8217b4369a0af4244d1fca")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("4999"), T("54fe976ba170c19ebae453679b362263")],
                            &[T("4998"), T("2cbd9c540641923027adb8ab89decc05")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("c4ca4238a0b923820dcc509a6f75849b")],
                        ],
                        tag: "FETCH 1",
                    },
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

#[test]
fn test_column_definition_lists_over_record_expressions() {
    run_scripts(&[
        ScriptTest {
            name: "column definition lists over a record expression in FROM, from PG18's rangefuncs regress module",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "WITH a(b) AS (VALUES (ROW(1, 2, 3))) SELECT * FROM a, coalesce(b) AS c(d INT, e INT, f INT, g INT);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "function return row and query-specified return row do not match", detail: "Returned row contains 3 attributes, but query expects 4.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH a(b) AS (VALUES (ROW(1, 2, 3))) SELECT * FROM a, coalesce(b) AS c(d INT, e INT);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "function return row and query-specified return row do not match", detail: "Returned row contains 3 attributes, but query expects 2.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH a(b) AS (VALUES (ROW(1, 2, 3))) SELECT * FROM a, coalesce(b) AS c(d INT, e INT, f FLOAT);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "function return row and query-specified return row do not match", detail: "Returned type integer at ordinal position 3, but query expects double precision.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH a(b) AS (VALUES (ROW(1, 2, 3))) SELECT * FROM a, coalesce(b) AS c(d INT, e INT, f INT);",
                    expected: Expected::Rows {
                        columns: &[Column("b", RECORD), Column("d", INT4), Column("e", INT4), Column("f", INT4)],
                        rows: &[
                            &[T("(1,2,3)"), T("1"), T("2"), T("3")],
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
