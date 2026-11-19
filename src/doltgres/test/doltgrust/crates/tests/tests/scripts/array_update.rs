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
fn test_array_subscript_update() {
    run_scripts(&[
        ScriptTest {
            name: "array subscript updates",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, a int[]);",
                "INSERT INTO t VALUES (1,ARRAY[[1,2,3],[4,5,6]]),(2,ARRAY[1,2,3]),(3,NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET a[2][3]=60 WHERE id=1 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{{1,2,3},{4,5,60}}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[1:2][2:3]=ARRAY[[20,30],[50,60]] WHERE id=1 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{{1,20,30},{4,50,60}}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[6]=99 WHERE id=2 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{1,2,3,NULL,NULL,99}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[1][1]=7 WHERE id=3 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{{7}}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[:][2:3]=ARRAY[[2,3],[5,6]] WHERE id=1 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{{1,2,3},{4,5,6}}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[3][1]=7 WHERE id=1;",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "array subscript out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[1:2][1:2]=ARRAY[[9,8]] WHERE id=1;",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "source array too small", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[NULL]=7 WHERE id=2;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "array subscript in assignment must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE id=1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{{1,2,3},{4,5,6}}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[NULL:2]=NULL WHERE id=1;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "array subscript in assignment must not be null", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "array assignments from row expressions and subqueries",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY,a int[]);",
                "INSERT INTO t VALUES (1,ARRAY[[1,2,3],[4,5,6]]),(2,ARRAY[1,2,3,NULL,NULL,99]),(3,ARRAY[[7]]);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET a[id]=(SELECT 8) WHERE id=2 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{1,8,3,NULL,NULL,99}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[(SELECT 1):(SELECT 2)]=(SELECT ARRAY[9,10]) WHERE id=2 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{9,10,3,NULL,NULL,99}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[1:2]=NULL WHERE id=2 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{9,10,3,NULL,NULL,99}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[2:1]=NULL WHERE id=2 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{9,10,3,NULL,NULL,99}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[1:1][1:1]=ARRAY[NULL,99]::int[] WHERE id=1 RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{{NULL,2,3},{4,5,6}}")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE id=3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{{7}}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[2:1]=ARRAY[1] WHERE id=2;",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "upper bound cannot be less than lower bound", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[id]=(SELECT 8) WHERE id=2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE id=2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{9,8,3,NULL,NULL,99}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "empty array assignments require explicit bounds",
            set_up_script: &[
                "CREATE TABLE t (a int[]);",
                "INSERT INTO t VALUES (ARRAY[]::int[]);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET a[:]=ARRAY[1];",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "array slice subscript must provide both boundaries", detail: "When assigning to a slice of an empty array value, slice boundaries must be fully specified.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a[1:1]=NULL;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY)],
                        rows: &[
                            &[T("{}")],
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
fn test_array_subscript_overflow() {
    run_scripts(&[
        ScriptTest {
            name: "array assignments that would grow past the largest array",
            set_up_script: &[
                "CREATE TABLE au_t (pk INT PRIMARY KEY, f1 INT[]);",
                "INSERT INTO au_t VALUES (10, '[-2147483648:-2147483647]={1,2}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE au_t SET f1[2147483647] = 42 WHERE pk = 10;",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "array size exceeds the maximum allowed (134217727)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE au_t SET f1[2147483646:2147483647] = ARRAY[4,2] WHERE pk = 10;",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "array size exceeds the maximum allowed (134217727)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f1 FROM au_t;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4_ARRAY)],
                        rows: &[
                            &[T("[-2147483648:-2147483647]={1,2}")],
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
fn test_subscripting() {
    run_scripts(&[
        ScriptTest {
            name: "subscripts fetch and assign elements and slices of arrays, jsonb, and fixed-length types",
            set_up_script: &[
                "CREATE TABLE st (id INT PRIMARY KEY, a INT[], j JSONB, p POINT);",
                r#"INSERT INTO st VALUES (1, '{1,2,3}', '{"k": 1}', '(1,2)'), (2, NULL, NULL, NULL);"#,
                "CREATE TYPE pair AS (x INT, y INT);",
                "CREATE TABLE sc (c pair[]);",
                "INSERT INTO sc VALUES (ARRAY[ROW(1,2)::pair, ROW(3,4)::pair]);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT (ARRAY[1,2,3])[2] AS a, (ARRAY[1,2,3])[0] AS b, (ARRAY[1,2,3])[2:] AS c, (ARRAY[1,2,3])[:2] AS d, (ARRAY[1,2,3])[:] AS e;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4_ARRAY), Column("d", INT4_ARRAY), Column("e", INT4_ARRAY)],
                        rows: &[
                            &[T("2"), Null, T("{2,3}"), T("{1,2}"), T("{1,2,3}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ARRAY[[1,2],[3,4]])[2][1] AS a, (ARRAY[[1,2],[3,4]])[1:2][2] AS b, (ARRAY[[1,2],[3,4]])[2] AS c, ('[0:2]={7,8,9}'::int[])[0] AS d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4_ARRAY), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("3"), T("{{1,2},{3,4}}"), Null, T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ARRAY[1,2,3])[NULL] AS a, (ARRAY[1,2,3])[1:NULL] AS b, (NULL::int[])[1] AS c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4_ARRAY), Column("c", INT4)],
                        rows: &[
                            &[Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (point(1.5, 2.5))[1] AS a, (point(1.5, 2.5))[2] AS b, ('abc'::name)[0] AS c, (lseg '[(1,2),(3,4)]')[1] AS d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", FLOAT8), Column("b", FLOAT8), Column("c", CHAR), Column("d", POINT)],
                        rows: &[
                            &[T("2.5"), Null, T("a"), T("(3,4)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (point(1, 2))[0:1];",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "slices of fixed-length arrays not implemented", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (1)[1];",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "cannot subscript type integer because it does not support subscripting", position: 9, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ('{"a": {"b": [1, {"c": 2}]}}'::jsonb)['a']['b'][1]['c'] AS a, ('{"a": 1}'::jsonb)['b'] AS b, ('[1, 2, 3]'::jsonb)[-1] AS c, ('[1, 2]'::jsonb)['x'] AS d, ('{"a": 1}'::jsonb)[NULL] AS e;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", JSONB), Column("b", JSONB), Column("c", JSONB), Column("d", JSONB), Column("e", JSONB)],
                        rows: &[
                            &[T("2"), Null, T("3"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE st SET a[2] = 20, a[5] = 50, j['k'] = '"v"', j['n']['m'] = '1', j['arr'][2] = 'true', p[0] = 9 WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE st SET a[2:3] = '{7,8}', j[0] = '"z"', p[1] = 5 WHERE id = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, j, p FROM st ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4_ARRAY), Column("j", JSONB), Column("p", POINT)],
                        rows: &[
                            &[T("1"), T("{1,20,3,NULL,50}"), T(r#"{"k": "v", "n": {"m": 1}, "arr": [null, null, true]}"#), T("(9,2)")],
                            &[T("2"), T("[2:3]={7,8}"), T(r#"["z"]"#), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE st SET a[1:2] = '{1}' WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "source array too small", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE st SET a[NULL] = 1;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "array subscript in assignment must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE st SET j[NULL] = '1';",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "jsonb subscript in assignment must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE st SET j['k']['x'] = '1' WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot replace existing key", detail: "The path assumes key is a composite object, but it is a scalar value.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE st SET j['arr'][-9] = '1' WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "path element at position 2 is out of range: -9", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE st SET a[1][1] = 1 WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "wrong number of array subscripts", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE sc SET c[2].y = 40, c[3].x = 5;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c FROM sc;",
                    expected: Expected::Rows {
                        columns: &[Column("c", USER_DEFINED)],
                        rows: &[
                            &[T(r#"{"(1,2)","(3,40)","(5,)"}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE a INT[] := '{1,2}'; j JSONB := '{}'; BEGIN a[4] := 4; a[1:2] := '{8,9}'; j['x']['y'] := '[1]'; RAISE NOTICE '% %', a, j; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: r#"{8,9,NULL,4} {"x": {"y": [1]}}"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE a INT[]; BEGIN a[1][1] := 7; RAISE NOTICE '%', a; END $$;",
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: "{{7}}", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE a INT[] := '{}'; BEGIN a[2][3] := 7; RAISE NOTICE '%', a; END $$;",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "array subscript out of range", ..E }),
                    skip: Some("needs PL/pgSQL's expanded-array assignment semantics, which refuse a second dimension on an empty array"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_array_coercion() {
    run_scripts(&[
        ScriptTest {
            name: "array casts coerce each element, with the target's type modifier",
            set_up_script: &[
                "CREATE TABLE act (f char(5)[], g varchar(5)[], n numeric(4,1)[]);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT '{1,2,3}'::text[]::int[] AS a, ARRAY[1,2,3]::text[]::int[]::float8[] AS b, ARRAY[1.5,2.5]::int[] AS c, '{{1,2},{3,4}}'::int[]::numeric[] AS d, '[0:1]={a,NULL}'::text[]::varchar[] AS e;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY), Column("b", FLOAT8_ARRAY), Column("c", INT4_ARRAY), Column("d", NUMERIC_ARRAY), Column("e", VARCHAR_ARRAY)],
                        rows: &[
                            &[T("{1,2,3}"), T("{1,2,3}"), T("{2,3}"), T("{{1,2},{3,4}}"), T("[0:1]={a,NULL}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY['abcdef','xy']::varchar(3)[] AS a, ARRAY['ab']::char(4)[] AS b, ARRAY[1.234, 5.6789]::numeric(5,2)[] AS c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR_ARRAY), Column("b", BPCHAR_ARRAY), Column("c", NUMERIC_ARRAY)],
                        rows: &[
                            &[T("{abc,xy}"), T(r#"{"ab  "}"#), T("{1.23,5.68}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1,x}'::text[]::int[];",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "x""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY[1.1] || ARRAY[2,3,4];",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", NUMERIC_ARRAY)],
                        rows: &[
                            &[T("{1.1,2,3,4}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT NULL::text[]::int[] AS a, '{}'::text[]::int[] AS b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4_ARRAY), Column("b", INT4_ARRAY)],
                        rows: &[
                            &[Null, T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO act VALUES ('{a,bb}', '{abc}', '{1.25,2}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO act VALUES ('{abcdef}', NULL, NULL);",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character(5)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO act VALUES (NULL, '{abcdef}', NULL);",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character varying(5)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f, g, n FROM act;",
                    expected: Expected::Rows {
                        columns: &[Column("f", BPCHAR_ARRAY), Column("g", VARCHAR_ARRAY), Column("n", NUMERIC_ARRAY)],
                        rows: &[
                            &[T(r#"{"a    ","bb   "}"#), T("{abc}"), T("{1.3,2.0}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE acp(text[]) AS SELECT $1::int[];",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE acp('{4,5}');",
                    expected: Expected::Rows {
                        columns: &[Column("int4", INT4_ARRAY)],
                        rows: &[
                            &[T("{4,5}")],
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
