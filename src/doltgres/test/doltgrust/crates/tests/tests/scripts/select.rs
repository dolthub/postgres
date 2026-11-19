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
fn test_select() {
    run_scripts(&[
        ScriptTest {
            name: "SELECT empty",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT;",
                    expected: Expected::Rows {
                        columns: &[],
                        rows: &[
                            &[],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT DISTINCT ON",
            set_up_script: &[
                "CREATE TABLE test (v1 INT4, v2 INT4);",
                "INSERT INTO test VALUES (1, 3), (1, 4), (2, 3), (2, 4);",
                "CREATE TABLE test2 (v1 INT4, v2 INT4, v3 INT4);",
                "INSERT INTO test2 VALUES (1, 3, 5), (2, 3, 5), (1, 4, 5), (2, 4, 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY v1, v2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("1"), T("4")],
                            &[T("2"), T("3")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT * FROM test ORDER BY v1, v2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("1"), T("4")],
                            &[T("2"), T("3")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v1) * FROM test ORDER BY v1, v2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v2) * FROM test ORDER BY v2, v1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("1"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v1) * FROM test ORDER BY v2, v1;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "SELECT DISTINCT ON expressions must match initial ORDER BY expressions", position: 20, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v2) * FROM test ORDER BY v2 DESC, v1 DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("2"), T("4")],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v2, v1) * FROM test2 ORDER BY v1, v2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4), Column("v3", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("5")],
                            &[T("1"), T("4"), T("5")],
                            &[T("2"), T("3"), T("5")],
                            &[T("2"), T("4"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v2, v1) * FROM test2 ORDER BY v1, v2 DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4), Column("v3", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("5")],
                            &[T("1"), T("3"), T("5")],
                            &[T("2"), T("4"), T("5")],
                            &[T("2"), T("3"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v2, v1) * FROM test2 ORDER BY v1, v2 LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4), Column("v3", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v2, v1) * FROM test2 ORDER BY v1, v2 DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4), Column("v3", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v1, v2, v3) * FROM test2 ORDER BY v1, v2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4), Column("v3", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("5")],
                            &[T("1"), T("4"), T("5")],
                            &[T("2"), T("3"), T("5")],
                            &[T("2"), T("4"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v3) v1 FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("a table without a primary key returns its rows in Doltgrust's row hash order rather than Postgres' insertion order (Appendix C, order of rows from a scan of a table without ORDER BY)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v1, v3) * FROM test2 ORDER BY v1, v2;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "SELECT DISTINCT ON expressions must match initial ORDER BY expressions", position: 24, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON(v2) * FROM test2 ORDER BY v1, v2;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "SELECT DISTINCT ON expressions must match initial ORDER BY expressions", position: 20, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "VALUES NULL type inference with ORDER BY",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT v FROM (VALUES (NULL), (2), (1)) AS t(v) ORDER BY v + 0 ASC NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[Null],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM (VALUES (NULL), (2), (1)) AS t(v) ORDER BY v + 0 ASC NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM (VALUES (NULL), (2), (1)) AS t(v) ORDER BY v + 0 DESC NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[Null],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM (VALUES (NULL), (2), (1)) AS t(v) ORDER BY v + 0 DESC NULLS LAST;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("1")],
                            &[Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ORDER BY NULL ordering",
            set_up_script: &[
                "CREATE TABLE null_ordering (id INT4 PRIMARY KEY, a INT4, b INT4);",
                "INSERT INTO null_ordering VALUES (1, NULL, 1), (2, NULL, NULL), (3, 1, 1), (4, 1, NULL), (5, 2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id FROM null_ordering ORDER BY a, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM null_ordering ORDER BY a ASC NULLS FIRST, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
                    query: "SELECT id FROM null_ordering ORDER BY a ASC NULLS LAST, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM null_ordering ORDER BY a NULLS FIRST, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
                    query: "SELECT id FROM null_ordering ORDER BY a NULLS LAST, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM null_ordering ORDER BY a DESC, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("5")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM null_ordering ORDER BY a DESC NULLS FIRST, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("5")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM null_ordering ORDER BY a DESC NULLS LAST, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("3")],
                            &[T("4")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM null_ordering ORDER BY a ASC NULLS LAST, b DESC NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("3")],
                            &[T("5")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, row_number() OVER (ORDER BY a ASC NULLS LAST, id) FROM null_ordering ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("row_number", INT8)],
                        rows: &[
                            &[T("1"), T("4")],
                            &[T("2"), T("5")],
                            &[T("3"), T("1")],
                            &[T("4"), T("2")],
                            &[T("5"), T("3")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_agg(id ORDER BY a DESC NULLS FIRST, id) FROM null_ordering;",
                    expected: Expected::Rows {
                        columns: &[Column("array_agg", INT4_ARRAY)],
                        rows: &[
                            &[T("{1,2,5,3,4}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "select large limit",
            assertions: &[
                ScriptTestAssertion {
                    query: "select 1 limit 18446744073709551615",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "bigint out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select 1 limit -18446744073709551616",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "bigint out of range", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "select values",
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from (values(1,'峰哥',18),(2,'王哥',20),(3,'张哥',22));",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4), Column("column2", TEXT), Column("column3", INT4)],
                        rows: &[
                            &[T("1"), T("峰哥"), T("18")],
                            &[T("2"), T("王哥"), T("20")],
                            &[T("3"), T("张哥"), T("22")],
                        ],
                        tag: "SELECT 3",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from (values(1,'峰哥',18),(2,'王哥',20),(3,'张哥',22)) x(id,name,age);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("age", INT4)],
                        rows: &[
                            &[T("1"), T("峰哥"), T("18")],
                            &[T("2"), T("王哥"), T("20")],
                            &[T("3"), T("张哥"), T("22")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from (values(1,'峰哥',18),(2,'王哥',20),(3,'张哥',22)) x(id,name,age) limit $1;",
                    bind_vars: &[BindVar::Int(2)],
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("age", INT4)],
                        rows: &[
                            &[T("1"), T("峰哥"), T("18")],
                            &[T("2"), T("王哥"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SELECT with no select expressions",
            set_up_script: &[
                "CREATE TABLE mytable (pk int primary key);",
                "INSERT INTO mytable VALUES (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select from mytable;",
                    expected: Expected::Rows {
                        columns: &[],
                        rows: &[
                            &[],
                            &[],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT FROM mytable where pk > 0);",
                    expected: Expected::Rows {
                        columns: &[Column("exists", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "String literal alias preserved in column name",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 42 as z, 'bob' as bob;",
                    expected: Expected::Rows {
                        columns: &[Column("z", INT4), Column("bob", TEXT)],
                        rows: &[
                            &[T("42"), T("bob")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'hello';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("hello")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a', 'b';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("a"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"select 'select 1 as ones', 'select x.y, x.y*2 as double from generate_series(1,4) as x(y)'
union all
select 'drop table gexec_test', NULL
union all
select 'drop table gexec_test', 'select ''2000-01-01''::date as party_over'"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("select 1 as ones"), T("select x.y, x.y*2 as double from generate_series(1,4) as x(y)")],
                            &[T("drop table gexec_test"), Null],
                            &[T("drop table gexec_test"), T("select '2000-01-01'::date as party_over")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "column names of schema-qualified functions",
            set_up_script: &[
                "CREATE SCHEMA function_labels;",
                "CREATE FUNCTION public.label_fn(integer) RETURNS integer LANGUAGE SQL AS 'SELECT $1 + 1';",
                "CREATE FUNCTION function_labels.label_fn(integer) RETURNS integer LANGUAGE SQL AS 'SELECT $1 + 2';",
                r#"CREATE FUNCTION function_labels."label.fn"(integer) RETURNS integer LANGUAGE SQL AS 'SELECT $1 + 3';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT public.label_fn(1), function_labels.label_fn(1);",
                    expected: Expected::Rows {
                        columns: &[Column("label_fn", INT4), Column("label_fn", INT4)],
                        rows: &[
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT function_labels.label_fn(1) AS custom_label;",
                    expected: Expected::Rows {
                        columns: &[Column("custom_label", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT function_labels."label.fn"(1);"#,
                    expected: Expected::Rows {
                        columns: &[Column("label.fn", INT4)],
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
        ScriptTest {
            name: "column names of functions called in FROM",
            set_up_script: &[
                "CREATE TABLE ft (id integer primary key);",
                "INSERT INTO ft VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relid FROM pg_partition_ancestors('ft');",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_partition_ancestors.relid FROM pg_partition_ancestors('ft');",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x FROM pg_partition_ancestors('ft') AS x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", REGCLASS)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relid FROM ft, pg_partition_ancestors('ft');",
                    expected: Expected::Rows {
                        columns: &[Column("relid", REGCLASS)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT unnest FROM unnest(ARRAY[1]);",
                    expected: Expected::Rows {
                        columns: &[Column("unnest", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "derived table with duplicate column names",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT 1 AS a, 'x' AS a) t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT *, ROW_NUMBER() OVER () AS n FROM (SELECT 1 AS a, 'x' AS a) t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", TEXT), Column("n", INT8)],
                        rows: &[
                            &[T("1"), T("x"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT 1, 2) t(a, a);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
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
fn test_collate_clauses() {
    run_scripts(&[
        ScriptTest {
            name: "COLLATE clauses",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT 'a' COLLATE "C", 'b' COLLATE pg_catalog.default, 'c' COLLATE "POSIX", pg_typeof('a' COLLATE "C");"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT), Column("?column?", TEXT), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("a"), T("b"), T("c"), T("unknown")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'a' COLLATE "nope";"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"collation "nope" for encoding "UTF8" does not exist"#, position: 12, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 1 COLLATE "C";"#,
                    expected: Expected::Error(Diagnostic { code: "42804", message: "collations are not supported by type integer", position: 10, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'b' < 'a' COLLATE "C", 'B' < 'a' COLLATE "C";"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT x FROM (VALUES ('b'), ('A'), ('a')) v(x) ORDER BY x COLLATE "C";"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", TEXT)],
                        rows: &[
                            &[T("A")],
                            &[T("a")],
                            &[T("b")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'a' COLLATE ucs_basic, 'a'::name COLLATE "C", ARRAY['a'] COLLATE "C";"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("name", NAME), Column("array", TEXT_ARRAY)],
                        rows: &[
                            &[T("a"), T("a"), T("{a}")],
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
fn test_limit_alias_and_distinct_on_rules() {
    run_scripts(&[
        ScriptTest {
            name: "LIMIT and OFFSET convert their arguments by assignment",
            assertions: &[
                ScriptTestAssertion {
                    query: "select 1 limit 18446744073709551615;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "bigint out of range", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select 1 limit -18446744073709551616;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "bigint out of range", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select 1 limit 2.5;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select 1 limit 'x';",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type bigint: "x""#, position: 16, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select 1 limit '2';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select 1 limit true;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of LIMIT must be type bigint, not type boolean", position: 16, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select 1 offset 1.5;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select 1 limit null;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "subqueries and VALUES in FROM need aliases",
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from (values(1,'a',18),(2,'b',20));",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4), Column("column2", TEXT), Column("column3", INT4)],
                        rows: &[
                            &[T("1"), T("a"), T("18")],
                            &[T("2"), T("b"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from (select 1);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from ( SELECT 1 ), (values (1));",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4), Column("column1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DISTINCT ON matches the ORDER BY prefix in any order",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE d (v1 INT4 PRIMARY KEY, v2 INT4, v3 INT4);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (1, 3, 5), (2, 3, 6), (3, 4, 5), (4, 4, 6);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON (v2, v3) * FROM d ORDER BY v3, v2, v1 DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4), Column("v3", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("5")],
                            &[T("3"), T("4"), T("5")],
                            &[T("2"), T("3"), T("6")],
                            &[T("4"), T("4"), T("6")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON (v3) * FROM d ORDER BY v2, v3;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "SELECT DISTINCT ON expressions must match initial ORDER BY expressions", position: 21, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON (v2, v1) * FROM d ORDER BY v2;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT4), Column("v2", INT4), Column("v3", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("5")],
                            &[T("2"), T("3"), T("6")],
                            &[T("3"), T("4"), T("5")],
                            &[T("4"), T("4"), T("6")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DISTINCT ON (v2, v1) * FROM d ORDER BY v3, v2;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "SELECT DISTINCT ON expressions must match initial ORDER BY expressions", position: 21, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_equality_join_rules() {
    run_scripts(&[
        ScriptTest {
            name: "joins on equal values",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE jl (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE jr (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO jl VALUES (1, 'x'), (2, 'y'), (NULL, 'z'), (2, 'w');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO jr VALUES (2, 'y'), (1, 'q'), (NULL, 'z'), (3, 'w'), (2, 'y2');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jl JOIN jr ON jl.a = jr.a ORDER BY 1, 2, 3, 4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x"), T("1"), T("q")],
                            &[T("2"), T("w"), T("2"), T("y")],
                            &[T("2"), T("w"), T("2"), T("y2")],
                            &[T("2"), T("y"), T("2"), T("y")],
                            &[T("2"), T("y"), T("2"), T("y2")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jl LEFT JOIN jr ON jl.a = jr.a AND jl.b = jr.b ORDER BY 1, 2, 3, 4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x"), Null, Null],
                            &[T("2"), T("w"), Null, Null],
                            &[T("2"), T("y"), T("2"), T("y")],
                            &[Null, T("z"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jl FULL JOIN jr ON jr.a = jl.a + 0 ORDER BY 1, 2, 3, 4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x"), T("1"), T("q")],
                            &[T("2"), T("w"), T("2"), T("y")],
                            &[T("2"), T("w"), T("2"), T("y2")],
                            &[T("2"), T("y"), T("2"), T("y")],
                            &[T("2"), T("y"), T("2"), T("y2")],
                            &[Null, T("z"), Null, Null],
                            &[Null, Null, T("3"), T("w")],
                            &[Null, Null, Null, T("z")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM jl RIGHT JOIN jr USING (b) ORDER BY 1, 2, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("a", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("q"), Null, T("1")],
                            &[T("w"), T("2"), T("3")],
                            &[T("y"), T("2"), T("2")],
                            &[T("y2"), Null, T("2")],
                            &[T("z"), Null, Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM jl JOIN jr ON jl.a::bigint = jr.a::smallint;",
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
fn test_select_into() {
    run_scripts(&[
        ScriptTest {
            name: "select into",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE src (f1 int4);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO src VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f1 INTO newt FROM src;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM newt;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT f1 INTO x FROM src) s;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "SELECT ... INTO is not allowed here", position: 31, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE int4_tbl (f1 int4);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO int4_tbl SELECT 1 INTO f;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "SELECT ... INTO is not allowed here", position: 36, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM int4_tbl;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 AS a INTO TEMP tmp1;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tmp1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 2 AS b INTO UNLOGGED u1;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * INTO newt FROM src;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "newt" already exists"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_strict_null_constants() {
    run_scripts(&[
        ScriptTest {
            name: "strict operators on NULL constants",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE tab0(pk INTEGER PRIMARY KEY, col0 INTEGER, col3 INTEGER, col4 FLOAT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tab0 VALUES (1, 2000000, 30000, 1.5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT - 76 AS col4 FROM tab0 AS cor0 WHERE NULL BETWEEN - col0 * + - 73 * 37 * col3 AND NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("col4", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ALL * FROM tab0 WHERE ( NULL ) <> - 39 * - col4 / + 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col0", INT4), Column("col3", INT4), Column("col4", FLOAT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tab0 WHERE NULL = 1/0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tab0 WHERE col0 * col3 * 1000 > 0;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "integer out of range", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tab0 WHERE NULL IS DISTINCT FROM col0 / 0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_joins_looking_up_primary_keys() {
    run_scripts(&[
        ScriptTest {
            name: "joins that look rows up by primary key",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE jl (id INT PRIMARY KEY, v TEXT, w INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE jr (id INT PRIMARY KEY, lid BIGINT, u TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE js (k SMALLINT PRIMARY KEY, name TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO jl SELECT i, 'v' || i, i % 10 FROM generate_series(1, 64) i;",
                    expected: Expected::Tag("INSERT 0 64"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO jr SELECT i, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 80 END, 'u' || i FROM generate_series(1, 300) i;",
                    expected: Expected::Tag("INSERT 0 300"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO js SELECT i, 'n' || i FROM generate_series(1, 20) i;",
                    expected: Expected::Tag("INSERT 0 20"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(l.w) FROM jr r JOIN jl l ON l.id = r.lid;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("223"), T("965")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.id, l.v FROM jr r JOIN jl l ON l.id = r.lid WHERE r.id < 20 ORDER BY r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("v1")],
                            &[T("2"), T("v2")],
                            &[T("3"), T("v3")],
                            &[T("4"), T("v4")],
                            &[T("5"), T("v5")],
                            &[T("6"), T("v6")],
                            &[T("7"), T("v7")],
                            &[T("8"), T("v8")],
                            &[T("10"), T("v10")],
                            &[T("11"), T("v11")],
                            &[T("12"), T("v12")],
                            &[T("13"), T("v13")],
                            &[T("14"), T("v14")],
                            &[T("15"), T("v15")],
                            &[T("16"), T("v16")],
                            &[T("17"), T("v17")],
                            &[T("19"), T("v19")],
                        ],
                        tag: "SELECT 17",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.id, l.v FROM jr r LEFT JOIN jl l ON l.id = r.lid WHERE r.id BETWEEN 60 AND 75 ORDER BY r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("60"), T("v60")],
                            &[T("61"), T("v61")],
                            &[T("62"), T("v62")],
                            &[T("63"), Null],
                            &[T("64"), T("v64")],
                            &[T("65"), Null],
                            &[T("66"), Null],
                            &[T("67"), Null],
                            &[T("68"), Null],
                            &[T("69"), Null],
                            &[T("70"), Null],
                            &[T("71"), Null],
                            &[T("72"), Null],
                            &[T("73"), Null],
                            &[T("74"), Null],
                            &[T("75"), Null],
                        ],
                        tag: "SELECT 16",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.id, l.v FROM jr r JOIN jl l ON l.id = r.lid AND l.w > 5 WHERE r.id < 40 ORDER BY r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("6"), T("v6")],
                            &[T("7"), T("v7")],
                            &[T("8"), T("v8")],
                            &[T("16"), T("v16")],
                            &[T("17"), T("v17")],
                            &[T("19"), T("v19")],
                            &[T("26"), T("v26")],
                            &[T("28"), T("v28")],
                            &[T("29"), T("v29")],
                            &[T("37"), T("v37")],
                            &[T("38"), T("v38")],
                            &[T("39"), T("v39")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM jr r JOIN (SELECT * FROM jl WHERE w = 3) l ON l.id = r.lid;",
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
                    query: "SELECT l.id, r.id FROM jl l JOIN jr r ON r.lid = l.id WHERE l.id < 5 ORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("161")],
                            &[T("1"), T("241")],
                            &[T("2"), T("2")],
                            &[T("2"), T("82")],
                            &[T("2"), T("242")],
                            &[T("3"), T("3")],
                            &[T("3"), T("83")],
                            &[T("3"), T("163")],
                            &[T("4"), T("4")],
                            &[T("4"), T("84")],
                            &[T("4"), T("164")],
                            &[T("4"), T("244")],
                        ],
                        tag: "SELECT 13",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM jl l JOIN jr r ON r.lid = l.id;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("223")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.name, l.v FROM js s JOIN jl l ON l.id = s.k ORDER BY s.k;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("v", TEXT)],
                        rows: &[
                            &[T("n1"), T("v1")],
                            &[T("n2"), T("v2")],
                            &[T("n3"), T("v3")],
                            &[T("n4"), T("v4")],
                            &[T("n5"), T("v5")],
                            &[T("n6"), T("v6")],
                            &[T("n7"), T("v7")],
                            &[T("n8"), T("v8")],
                            &[T("n9"), T("v9")],
                            &[T("n10"), T("v10")],
                            &[T("n11"), T("v11")],
                            &[T("n12"), T("v12")],
                            &[T("n13"), T("v13")],
                            &[T("n14"), T("v14")],
                            &[T("n15"), T("v15")],
                            &[T("n16"), T("v16")],
                            &[T("n17"), T("v17")],
                            &[T("n18"), T("v18")],
                            &[T("n19"), T("v19")],
                            &[T("n20"), T("v20")],
                        ],
                        tag: "SELECT 20",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT r.id, s.name FROM jr r JOIN js s ON s.k = r.lid WHERE r.id < 30 ORDER BY r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("n1")],
                            &[T("2"), T("n2")],
                            &[T("3"), T("n3")],
                            &[T("4"), T("n4")],
                            &[T("5"), T("n5")],
                            &[T("6"), T("n6")],
                            &[T("7"), T("n7")],
                            &[T("8"), T("n8")],
                            &[T("10"), T("n10")],
                            &[T("11"), T("n11")],
                            &[T("12"), T("n12")],
                            &[T("13"), T("n13")],
                            &[T("14"), T("n14")],
                            &[T("15"), T("n15")],
                            &[T("16"), T("n16")],
                            &[T("17"), T("n17")],
                            &[T("19"), T("n19")],
                            &[T("20"), T("n20")],
                        ],
                        tag: "SELECT 18",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM jr a JOIN jr b ON a.id = b.id;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("300")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM (SELECT a.id, a.u FROM jr a, jr b WHERE a.id = b.id LIMIT 7) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM jl a JOIN jl b ON b.id = a.w;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("58")],
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
fn test_alias_column_counts() {
    run_scripts(&[
        ScriptTest {
            name: "aliases that name more columns than a FROM item has",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE alias_t (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO alias_t VALUES (1, 'x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM alias_t v(p, q);",
                    expected: Expected::Rows {
                        columns: &[Column("p", INT4), Column("q", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p FROM alias_t v(p);",
                    expected: Expected::Rows {
                        columns: &[Column("p", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM alias_t v(p, q, r);",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"table "v" has 2 columns available but 3 columns specified"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 2) g(n);",
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
                    query: "SELECT * FROM generate_series(1, 2) g(n, m);",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"table "g" has 1 columns available but 2 columns specified"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (VALUES (1, 2)) v(a, b, c);",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"table "v" has 2 columns available but 3 columns specified"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT 1) s(a, b);",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"table "s" has 1 columns available but 2 columns specified"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_outer_join_reduction() {
    run_scripts(&[
        ScriptTest {
            name: "Outer joins that WHERE conditions reduce",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE oj_l (id INT PRIMARY KEY, k INT, t TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE oj_r (id INT PRIMARY KEY, k INT, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oj_l VALUES (1, 1, 'a'), (2, 2, 'b'), (3, NULL, 'c'), (4, 4, NULL);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oj_r VALUES (10, 1, 100), (11, 2, NULL), (12, 5, 50);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l LEFT JOIN oj_r r ON l.k = r.k WHERE r.v > 10 ORDER BY l.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l LEFT JOIN oj_r r ON l.k = r.k WHERE r.v IS NULL ORDER BY l.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("2"), T("11")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l LEFT JOIN oj_r r ON l.k = r.k WHERE r.v + 1 > 10 OR r.id = 11 ORDER BY l.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l LEFT JOIN oj_r r ON l.k = r.k WHERE r.v > 10 OR l.id = 3 ORDER BY l.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l LEFT JOIN oj_r r ON l.k = r.k WHERE coalesce(r.v, 0) = 0 ORDER BY l.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("2"), T("11")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l RIGHT JOIN oj_r r ON l.k = r.k WHERE l.t <> 'z' ORDER BY r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l FULL JOIN oj_r r ON l.k = r.k WHERE l.t IS NOT NULL ORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("11")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l FULL JOIN oj_r r ON l.k = r.k WHERE r.v::text LIKE '1%' ORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l FULL JOIN oj_r r ON l.k = r.k WHERE l.id < 4 AND r.v >= 0 ORDER BY l.id, r.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l LEFT JOIN oj_r r ON l.k = r.k WHERE (r.v > 10) IS NOT TRUE ORDER BY l.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("2"), T("11")],
                            &[T("3"), Null],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l.id, r.id FROM oj_l l LEFT JOIN oj_r r ON l.k = r.k WHERE NOT (r.v > 10) ORDER BY l.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[],
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
fn test_join_order_search() {
    run_scripts(&[
        ScriptTest {
            name: "Joins of several tables in any order",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE jo_a (id INT PRIMARY KEY, b_id INT, t TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE jo_b (id INT PRIMARY KEY, c_id INT, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE jo_c (id INT PRIMARY KEY, d_id INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE jo_d (id INT PRIMARY KEY, name TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO jo_a SELECT g, g % 30, 't' || g FROM generate_series(1, 300) g;",
                    expected: Expected::Tag("INSERT 0 300"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO jo_b SELECT g, g % 10, g * 2 FROM generate_series(0, 29) g;",
                    expected: Expected::Tag("INSERT 0 30"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO jo_c SELECT g, g % 3 FROM generate_series(0, 9) g;",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO jo_d VALUES (0, 'zero'), (1, 'one'), (2, 'two');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.name, count(*) FROM jo_a a JOIN jo_b b ON a.b_id = b.id JOIN jo_c c ON b.c_id = c.id JOIN jo_d d ON c.d_id = d.id GROUP BY d.name ORDER BY d.name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("one"), T("90")],
                            &[T("two"), T("90")],
                            &[T("zero"), T("120")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.id, d.name FROM jo_d d JOIN jo_c c ON c.d_id = d.id AND d.name = 'two' JOIN jo_b b ON b.c_id = c.id JOIN jo_a a ON a.b_id = b.id AND a.id < 40 ORDER BY a.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("2"), T("two")],
                            &[T("5"), T("two")],
                            &[T("8"), T("two")],
                            &[T("12"), T("two")],
                            &[T("15"), T("two")],
                            &[T("18"), T("two")],
                            &[T("22"), T("two")],
                            &[T("25"), T("two")],
                            &[T("28"), T("two")],
                            &[T("32"), T("two")],
                            &[T("35"), T("two")],
                            &[T("38"), T("two")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM jo_a a, jo_b b, jo_c c WHERE a.b_id = b.id AND b.c_id = c.id AND c.d_id = 1 AND a.t LIKE 't1%';",
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
                    query: "SELECT count(*) FROM jo_a a JOIN jo_b b ON a.b_id = b.id JOIN jo_c c ON true JOIN jo_d d ON 1 = 1 WHERE c.id < 2 AND d.id = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("600")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM jo_a a JOIN jo_b b ON a.b_id = b.id JOIN jo_c c ON false;",
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
                    query: "SELECT a.id, b.id, c.id FROM jo_a a JOIN jo_b b ON a.b_id = b.id JOIN jo_c c ON b.c_id = c.id AND a.id = c.id ORDER BY a.id LIMIT 5;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                            &[T("3"), T("3"), T("3")],
                            &[T("4"), T("4"), T("4")],
                            &[T("5"), T("5"), T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x.*, y.* FROM (SELECT 1 AS p) x JOIN jo_d y ON y.id = x.p JOIN jo_c z ON z.d_id = y.id ORDER BY z.id;",
                    expected: Expected::Rows {
                        columns: &[Column("p", INT4), Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("one")],
                            &[T("1"), T("1"), T("one")],
                            &[T("1"), T("1"), T("one")],
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
fn test_implied_equalities() {
    run_scripts(&[
        ScriptTest {
            name: "Equalities implied through joins",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE eq_a (id INT PRIMARY KEY, x INT, t TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE eq_b (id INT PRIMARY KEY, y INT, u TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX eq_a_x ON eq_a (x);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO eq_a SELECT g, g % 20, 'a' || g FROM generate_series(1, 200) g;",
                    expected: Expected::Tag("INSERT 0 200"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO eq_b SELECT g, g % 25, 'b' || (g % 3) FROM generate_series(1, 100) g;",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.id, b.id FROM eq_a a JOIN eq_b b ON a.x = b.y WHERE b.y = 5 ORDER BY a.id, b.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("5"), T("5")],
                            &[T("5"), T("30")],
                            &[T("5"), T("55")],
                            &[T("5"), T("80")],
                            &[T("25"), T("5")],
                            &[T("25"), T("30")],
                            &[T("25"), T("55")],
                            &[T("25"), T("80")],
                            &[T("45"), T("5")],
                            &[T("45"), T("30")],
                            &[T("45"), T("55")],
                            &[T("45"), T("80")],
                            &[T("65"), T("5")],
                            &[T("65"), T("30")],
                            &[T("65"), T("55")],
                            &[T("65"), T("80")],
                            &[T("85"), T("5")],
                            &[T("85"), T("30")],
                            &[T("85"), T("55")],
                            &[T("85"), T("80")],
                            &[T("105"), T("5")],
                            &[T("105"), T("30")],
                            &[T("105"), T("55")],
                            &[T("105"), T("80")],
                            &[T("125"), T("5")],
                            &[T("125"), T("30")],
                            &[T("125"), T("55")],
                            &[T("125"), T("80")],
                            &[T("145"), T("5")],
                            &[T("145"), T("30")],
                            &[T("145"), T("55")],
                            &[T("145"), T("80")],
                            &[T("165"), T("5")],
                            &[T("165"), T("30")],
                            &[T("165"), T("55")],
                            &[T("165"), T("80")],
                            &[T("185"), T("5")],
                            &[T("185"), T("30")],
                            &[T("185"), T("55")],
                            &[T("185"), T("80")],
                        ],
                        tag: "SELECT 40",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM eq_a a, eq_b b WHERE a.x = b.y AND a.x = 7 AND b.y = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("40")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM eq_a a, eq_b b WHERE a.x = b.y AND a.x = 7 AND b.y = 8;",
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
                    query: "SELECT count(*) FROM eq_a a JOIN eq_b b ON a.x = b.y AND b.y = 3 JOIN eq_a c ON c.x = a.x;",
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
                    query: "SELECT count(*) FROM eq_a a JOIN eq_b b ON a.x = b.y WHERE b.y = NULL;",
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
                    query: "SELECT count(*) FROM eq_a a JOIN eq_b b ON a.t = b.u WHERE b.u = 'b1';",
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
                    query: "SELECT count(*) FROM eq_a a JOIN eq_b b ON a.x = b.y WHERE b.y::bigint = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("40")],
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
fn test_lookup_join_placeholders() {
    run_scripts(&[
        ScriptTest {
            name: "Lookup joins whose inner relation computes a placeholder",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE lp_a (id INT PRIMARY KEY, b_id INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lp_b (id INT PRIMARY KEY, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lp_a VALUES (1, 10), (2, 20), (3, NULL);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lp_b SELECT g, g * 2 FROM generate_series(1, 2000) g;",
                    expected: Expected::Tag("INSERT 0 2000"),
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
                    query: "SELECT a.id, ss.flag FROM lp_a a LEFT JOIN (SELECT b.id, (b.id IS NOT NULL) AS flag FROM lp_b b) ss ON ss.id = a.b_id ORDER BY a.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("flag", BOOL)],
                        rows: &[
                            &[T("1"), T("t")],
                            &[T("2"), T("t")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.id, ss.flag FROM lp_a a LEFT JOIN (SELECT b.id, (b.id IS NOT NULL) AS flag FROM lp_b b WHERE b.v > 30) ss ON ss.id = a.b_id ORDER BY a.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("flag", BOOL)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("t")],
                            &[T("3"), Null],
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
fn test_bitmap_scans() {
    run_scripts(&[
        ScriptTest {
            name: "Bitmap scans of OR and AND clauses",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE bm (id INT PRIMARY KEY, k INT, c INT, t TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bm_k ON bm (k);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX bm_c ON bm (c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bm SELECT g, g % 1000, g % 97, 'x' || g FROM generate_series(1, 20000) g;",
                    expected: Expected::Tag("INSERT 0 20000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE bm;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM bm WHERE k BETWEEN 1 AND 3 OR k BETWEEN 500 AND 502;",
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
                    query: "SELECT count(*) FROM bm WHERE k = 5 OR c = 7;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("226")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bm WHERE k = 5 AND c = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("13005")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM bm WHERE k < 3 OR id < 3 ORDER BY id LIMIT 5;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("1000")],
                            &[T("1001")],
                            &[T("1002")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE bo (id INT PRIMARY KEY, x INT, y INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bo VALUES (1, 5, 7), (2, 900, 13), (3, NULL, 96);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bo.id, count(*) FROM bo JOIN bm ON bm.k = bo.x OR bm.c = bo.y GROUP BY bo.id ORDER BY bo.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("226")],
                            &[T("2"), T("226")],
                            &[T("3"), T("206")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bo.id, count(bm.id) FROM bo LEFT JOIN bm ON (bm.k = bo.x OR bm.c = bo.y) AND bm.id < 50 GROUP BY bo.id ORDER BY bo.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("1")],
                            &[T("3"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bo.id FROM bo WHERE EXISTS (SELECT 1 FROM bm WHERE (bm.k = bo.x OR bm.c = bo.y) AND bm.id < 10) ORDER BY bo.id;",
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
                    query: "SELECT bo.id FROM bo WHERE NOT EXISTS (SELECT 1 FROM bm WHERE (bm.k = bo.x OR bm.c = bo.y) AND bm.id < 10) ORDER BY bo.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
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
fn test_parameterized_index_scans() {
    run_scripts(&[
        ScriptTest {
            name: "Index scans by range join clauses, run again for each outer row",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE pj (id INT PRIMARY KEY, k INT, c INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pj_k ON pj (k);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pj_c ON pj (c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pj SELECT g, g % 1000, g % 97 FROM generate_series(1, 20000) g;",
                    expected: Expected::Tag("INSERT 0 20000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE pj;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE po (id INT PRIMARY KEY, x INT, y INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO po VALUES (1, 5, 7), (2, 900, 13), (3, NULL, 96);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashjoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT po.id, count(*) FROM po JOIN pj ON pj.k BETWEEN po.x AND po.x + 1 GROUP BY po.id ORDER BY po.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("40")],
                            &[T("2"), T("40")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT po.id, count(pj.id) FROM po LEFT JOIN pj ON pj.k < po.x - 895 AND pj.id < 3000 GROUP BY po.id ORDER BY po.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("14")],
                            &[T("3"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT po.id FROM po WHERE EXISTS (SELECT 1 FROM pj WHERE pj.c > po.y + 80) ORDER BY po.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT po.id FROM po WHERE NOT EXISTS (SELECT 1 FROM pj WHERE pj.c > po.y + 80) ORDER BY po.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
fn test_system_columns() {
    run_scripts(&[
        ScriptTest {
            name: "ctid, xmin, xmax, cmin, and cmax of scanned and changed rows",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE sc (a INT PRIMARY KEY, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sc VALUES (1, 'x'), (2, 'y');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, xmin = 0 AS xmin_zero, xmax, cmin, cmax, a FROM sc ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("xmin_zero", BOOL), Column("xmax", XID), Column("cmin", CID), Column("cmax", CID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("f"), T("0"), T("0"), T("0"), T("1")],
                            &[T("(0,2)"), T("f"), T("0"), T("0"), T("0"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sc VALUES (3, 'z') RETURNING ctid, xmax, cmin, cmax;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("xmax", XID), Column("cmin", CID), Column("cmax", CID)],
                        rows: &[
                            &[T("(0,3)"), T("0"), T("0"), T("0")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE sc SET b = 'w' WHERE a = 3 RETURNING ctid, xmax, cmin, cmax;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("xmax", XID), Column("cmin", CID), Column("cmax", CID)],
                        rows: &[
                            &[T("(0,4)"), T("0"), T("1"), T("1")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE sc SET b = 'v' WHERE a = 1 RETURNING ctid, xmax, cmin, cmax;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("xmax", XID), Column("cmin", CID), Column("cmax", CID)],
                        rows: &[
                            &[T("(0,5)"), T("0"), T("2"), T("2")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM sc WHERE a = 2 RETURNING ctid, xmax <> 0 AS has_xmax, cmin, cmax;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("has_xmax", BOOL), Column("cmin", CID), Column("cmax", CID)],
                        rows: &[
                            &[T("(0,2)"), T("t"), T("3"), T("3")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, xmax, cmin, cmax, a FROM sc ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("xmax", XID), Column("cmin", CID), Column("cmax", CID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,5)"), T("0"), T("2"), T("2"), T("1")],
                            &[T("(0,4)"), T("0"), T("1"), T("1"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM sc WHERE ctid = '(0,1)';",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.ctid, t.ctid FROM sc s JOIN sc t ON s.a = t.a ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("ctid", TID)],
                        rows: &[
                            &[T("(0,4)"), T("(0,4)")],
                            &[T("(0,5)"), T("(0,5)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM sc WHERE ctid > '(0,3)' ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid FROM (SELECT ctid FROM sc) s;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID)],
                        rows: &[
                            &[T("(0,1)")],
                            &[T("(0,2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT max(ctid), min(xmin::text::int) > 0 FROM sc;",
                    expected: Expected::Rows {
                        columns: &[Column("max", TID), Column("?column?", BOOL)],
                        rows: &[
                            &[T("(0,2)"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a FROM sc WHERE a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,2)"), T("2")],
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
fn test_tid_scans() {
    run_scripts(&[
        ScriptTest {
            name: "Scans of the rows a table's ctid conditions name",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE tt (a INT PRIMARY KEY, b TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO tt VALUES (1, 'r'), (2, 'r'), (3, 'r'), (4, 'r'), (5, 'r'), (6, 'r'), (7, 'r'), (8, 'r'), (9, 'r'), (10, 'r');"#,
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM tt WHERE ctid = '(0,3)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a, ctid FROM tt WHERE ctid = ANY ('{"(0,5)","(0,2)","(0,5)","(9,1)"}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("ctid", TID)],
                        rows: &[
                            &[T("2"), T("(0,2)")],
                            &[T("5"), T("(0,5)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM tt WHERE ctid = '(0,1)' OR ctid = '(0,4)' ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM tt WHERE ctid > '(0,7)' AND ctid <= '(0,9)' ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM tt WHERE ctid >= '(0,8)' AND ctid < '(1,0)' AND a <> 9 ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("8")],
                            &[T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v.t, tt.a FROM (VALUES ('(0,1)'::tid), ('(0,2)'::tid)) v(t) JOIN tt ON tt.ctid = v.t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("t", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM tt WHERE ctid = NULL;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM tt WHERE ctid = '(0,0)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE tt SET b = 'u' WHERE ctid = '(0,2)' RETURNING a, ctid;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("ctid", TID)],
                        rows: &[
                            &[T("2"), T("(0,11)")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM tt WHERE ctid = '(0,2)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM tt WHERE ctid >= '(0,11)' RETURNING a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_statement_timeout_cancels_a_running_query() {
    run_scripts(&[
        ScriptTest {
            name: "statement_timeout cancels a long join and the session goes on",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET statement_timeout = 300;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) a(x), (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) b(x), (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) c(x), (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) d(x), (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) e(x), (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) f(x), (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) g(x), (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) h(x);",
                    expected: Expected::Error(Diagnostic { code: "57014", message: "canceling statement due to statement timeout", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
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
fn test_incremental_sort() {
    run_scripts(&[
        ScriptTest {
            name: "Incremental Sort over input sorted by leading keys, in its full and presorted prefix modes",
            set_up_script: &[
                "CREATE TABLE ist (id int PRIMARY KEY, a int, b int, c text) WITH (autovacuum_enabled = false);",
                "INSERT INTO ist SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE i / 100 END, (i * 7919) % 101, 'r' || i FROM generate_series(1, 1000) i;",
                "INSERT INTO ist SELECT 1000 + i, 20, (i * 31) % 17, 'x' FROM generate_series(1, 300) i;",
                "CREATE TABLE ist2 (k int PRIMARY KEY, a int, v int) WITH (autovacuum_enabled = false);",
                "INSERT INTO ist2 SELECT i, i % 23, i FROM generate_series(1, 200) i;",
                "ANALYZE ist;",
                "ANALYZE ist2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT a, b FROM ist ORDER BY a) s ORDER BY a, b LIMIT 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Incremental Sort")],
                            &[T("        Sort Key: ist.a, ist.b")],
                            &[T("        Presorted Key: ist.a")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: ist.a")],
                            &[T("              ->  Seq Scan on ist")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a, b FROM ist ORDER BY a) s ORDER BY a, b LIMIT 10;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("0"), T("2")],
                            &[T("0"), T("3")],
                            &[T("0"), T("4")],
                            &[T("0"), T("5")],
                            &[T("0"), T("6")],
                            &[T("0"), T("7")],
                            &[T("0"), T("8")],
                            &[T("0"), T("9")],
                            &[T("0"), T("10")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a, b FROM ist ORDER BY a) s ORDER BY a, b LIMIT 55;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("0"), T("2")],
                            &[T("0"), T("3")],
                            &[T("0"), T("4")],
                            &[T("0"), T("5")],
                            &[T("0"), T("6")],
                            &[T("0"), T("7")],
                            &[T("0"), T("8")],
                            &[T("0"), T("9")],
                            &[T("0"), T("10")],
                            &[T("0"), T("11")],
                            &[T("0"), T("12")],
                            &[T("0"), T("13")],
                            &[T("0"), T("14")],
                            &[T("0"), T("15")],
                            &[T("0"), T("16")],
                            &[T("0"), T("17")],
                            &[T("0"), T("18")],
                            &[T("0"), T("19")],
                            &[T("0"), T("20")],
                            &[T("0"), T("21")],
                            &[T("0"), T("22")],
                            &[T("0"), T("23")],
                            &[T("0"), T("24")],
                            &[T("0"), T("25")],
                            &[T("0"), T("26")],
                            &[T("0"), T("27")],
                            &[T("0"), T("28")],
                            &[T("0"), T("29")],
                            &[T("0"), T("30")],
                            &[T("0"), T("31")],
                            &[T("0"), T("32")],
                            &[T("0"), T("33")],
                            &[T("0"), T("34")],
                            &[T("0"), T("35")],
                            &[T("0"), T("36")],
                            &[T("0"), T("37")],
                            &[T("0"), T("39")],
                            &[T("0"), T("40")],
                            &[T("0"), T("41")],
                            &[T("0"), T("42")],
                            &[T("0"), T("43")],
                            &[T("0"), T("44")],
                            &[T("0"), T("45")],
                            &[T("0"), T("46")],
                            &[T("0"), T("47")],
                            &[T("0"), T("48")],
                            &[T("0"), T("49")],
                            &[T("0"), T("50")],
                            &[T("0"), T("51")],
                            &[T("0"), T("52")],
                            &[T("0"), T("53")],
                            &[T("0"), T("54")],
                            &[T("0"), T("55")],
                            &[T("0"), T("56")],
                        ],
                        tag: "SELECT 55",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, count(*), sum(b), min(b), max(b) FROM (SELECT * FROM (SELECT a, b FROM ist ORDER BY a) s ORDER BY a, b LIMIT 70) q GROUP BY a ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8), Column("sum", INT8), Column("min", INT4), Column("max", INT4)],
                        rows: &[
                            &[T("0"), T("70"), T("2530"), T("1"), T("72")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, row_number() OVER () FROM (SELECT * FROM (SELECT a, b FROM ist ORDER BY a NULLS FIRST) s ORDER BY a NULLS FIRST, b DESC LIMIT 40) q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("row_number", INT8)],
                        rows: &[
                            &[Null, T("89"), T("1")],
                            &[Null, T("77"), T("2")],
                            &[Null, T("76"), T("3")],
                            &[Null, T("64"), T("4")],
                            &[Null, T("51"), T("5")],
                            &[Null, T("39"), T("6")],
                            &[Null, T("38"), T("7")],
                            &[Null, T("26"), T("8")],
                            &[Null, T("13"), T("9")],
                            &[Null, T("1"), T("10")],
                            &[T("0"), T("100"), T("11")],
                            &[T("0"), T("99"), T("12")],
                            &[T("0"), T("98"), T("13")],
                            &[T("0"), T("97"), T("14")],
                            &[T("0"), T("96"), T("15")],
                            &[T("0"), T("95"), T("16")],
                            &[T("0"), T("94"), T("17")],
                            &[T("0"), T("93"), T("18")],
                            &[T("0"), T("92"), T("19")],
                            &[T("0"), T("91"), T("20")],
                            &[T("0"), T("90"), T("21")],
                            &[T("0"), T("89"), T("22")],
                            &[T("0"), T("88"), T("23")],
                            &[T("0"), T("87"), T("24")],
                            &[T("0"), T("86"), T("25")],
                            &[T("0"), T("85"), T("26")],
                            &[T("0"), T("84"), T("27")],
                            &[T("0"), T("83"), T("28")],
                            &[T("0"), T("82"), T("29")],
                            &[T("0"), T("81"), T("30")],
                            &[T("0"), T("80"), T("31")],
                            &[T("0"), T("79"), T("32")],
                            &[T("0"), T("78"), T("33")],
                            &[T("0"), T("77"), T("34")],
                            &[T("0"), T("76"), T("35")],
                            &[T("0"), T("75"), T("36")],
                            &[T("0"), T("74"), T("37")],
                            &[T("0"), T("73"), T("38")],
                            &[T("0"), T("72"), T("39")],
                            &[T("0"), T("71"), T("40")],
                        ],
                        tag: "SELECT 40",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(x), hashtext(x) FROM (SELECT string_agg(a::text || ':' || b::text, ',') x FROM (SELECT * FROM (SELECT a, b FROM ist ORDER BY a) s ORDER BY a, b) q) z;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("hashtext", INT4)],
                        rows: &[
                            &[T("6486"), T("1569818863")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM (SELECT * FROM (SELECT a, b FROM ist ORDER BY a) s ORDER BY a, b OFFSET 1290) q;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[Null, T("1")],
                            &[Null, T("13")],
                            &[Null, T("26")],
                            &[Null, T("38")],
                            &[Null, T("39")],
                            &[Null, T("51")],
                            &[Null, T("64")],
                            &[Null, T("76")],
                            &[Null, T("77")],
                            &[Null, T("89")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(x), hashtext(x) FROM (SELECT string_agg(a::text || ':' || b::text, ',') x FROM (SELECT * FROM (SELECT a, b FROM ist WHERE a = 20 ORDER BY a) s ORDER BY a, b LIMIT 250) q) z;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("hashtext", INT4)],
                        rows: &[
                            &[T("1323"), T("1561778193")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), hashtext(string_agg(coalesce(a::text, 'n') || ':' || b::text, ',')) FROM (SELECT * FROM (SELECT a, b FROM ist ORDER BY a DESC) s ORDER BY a DESC, b DESC LIMIT 600) q;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("hashtext", INT4)],
                        rows: &[
                            &[T("600"), T("-187866214")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b, sum(b) OVER (PARTITION BY a ORDER BY b) FROM (SELECT a, b FROM ist ORDER BY a) s;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w1 AS (PARTITION BY ist.a ORDER BY ist.b)")],
                            &[T("  ->  Incremental Sort")],
                            &[T("        Sort Key: ist.a, ist.b")],
                            &[T("        Presorted Key: ist.a")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: ist.a")],
                            &[T("              ->  Seq Scan on ist")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(x), hashtext(x) FROM (SELECT string_agg(a::text || ':' || b::text || ':' || s::text, ',') x FROM (SELECT a, b, sum(b) OVER (PARTITION BY a ORDER BY b) s FROM (SELECT a, b FROM ist ORDER BY a) s) q) z;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("hashtext", INT4)],
                        rows: &[
                            &[T("12050"), T("354664114")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_incremental_sort = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT a, b FROM ist ORDER BY a) s ORDER BY a, b LIMIT 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: ist.a, ist.b")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: ist.a")],
                            &[T("              ->  Seq Scan on ist")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a, b FROM ist ORDER BY a) s ORDER BY a, b LIMIT 10;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("0"), T("2")],
                            &[T("0"), T("3")],
                            &[T("0"), T("4")],
                            &[T("0"), T("5")],
                            &[T("0"), T("6")],
                            &[T("0"), T("7")],
                            &[T("0"), T("8")],
                            &[T("0"), T("9")],
                            &[T("0"), T("10")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b, sum(b) OVER (PARTITION BY a ORDER BY b) FROM (SELECT a, b FROM ist ORDER BY a) s;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("WindowAgg")],
                            &[T("  Window: w1 AS (PARTITION BY ist.a ORDER BY ist.b)")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: ist.a, ist.b")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: ist.a")],
                            &[T("              ->  Seq Scan on ist")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_incremental_sort;",
                    expected: Expected::Tag("RESET"),
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
                    query: "EXPLAIN (COSTS OFF) SELECT x.a, x.b, y.v FROM (SELECT a, b FROM ist ORDER BY a) x JOIN ist2 y ON x.a = y.a AND x.b = y.v;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Merge Join")],
                            &[T("  Merge Cond: ((ist.a = y.a) AND (ist.b = y.v))")],
                            &[T("  ->  Incremental Sort")],
                            &[T("        Sort Key: ist.a, ist.b")],
                            &[T("        Presorted Key: ist.a")],
                            &[T("        ->  Sort")],
                            &[T("              Sort Key: ist.a")],
                            &[T("              ->  Seq Scan on ist")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: y.a, y.v")],
                            &[T("        ->  Seq Scan on ist2 y")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(y.v) FROM (SELECT a, b FROM ist ORDER BY a) x JOIN ist2 y ON x.a = y.a AND x.b = y.v;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("47"), T("2348")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashjoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_nestloop;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_ctid_blocks_within_the_relation_size() {
    run_scripts(&[
        ScriptTest {
            name: "every ctid's block is below the relation's size after deletes, so a scan of ctid ranges up to it finds every row",
            set_up_script: &[
                "CREATE TABLE cg (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO cg SELECT i, 'x' FROM generate_series(1, 70000) i;",
                "DELETE FROM cg WHERE id < 70000;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id FROM cg WHERE ctid >= '(0,0)' AND ctid < format('(%s,0)', pg_relation_size('cg') / current_setting('block_size')::int)::tid",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("70000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ctid::text::point)[0] < pg_relation_size('cg') / current_setting('block_size')::int AS covered FROM cg",
                    expected: Expected::Rows {
                        columns: &[Column("covered", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE cg",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT (ctid::text::point)[0] FROM cg) < relpages AS covered FROM pg_class WHERE relname = 'cg'",
                    expected: Expected::Rows {
                        columns: &[Column("covered", BOOL)],
                        rows: &[
                            &[T("t")],
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
fn test_ctids_of_a_table_made_again() {
    run_scripts(&[
        ScriptTest {
            name: "a table made again under a dropped table's name numbers its rows' ctids from the start, and its size and relpages count none of the dropped table's",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE fresh1 (id INT PRIMARY KEY)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fresh1 VALUES (1), (2)",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE fresh1",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fresh1 (id INT PRIMARY KEY)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fresh1 VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, id FROM fresh1",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE fresh1",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fresh1 (id INT PRIMARY KEY)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fresh1 VALUES (5), (6), (7)",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, id FROM fresh1 ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("5")],
                            &[T("(0,2)"), T("6")],
                            &[T("(0,3)"), T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, id FROM fresh1",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE cg (id INT PRIMARY KEY, v TEXT)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO cg SELECT i, 'x' FROM generate_series(1, 70000) i",
                    expected: Expected::Tag("INSERT 0 70000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE cg",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE cg (id INT PRIMARY KEY, v TEXT)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO cg VALUES (1, 'x')",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE cg",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cg.ctid, relpages, pg_relation_size('cg') / current_setting('block_size')::int AS pages FROM cg, pg_class WHERE relname = 'cg'",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("relpages", INT4), Column("pages", INT8)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("1")],
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
fn test_ctids_after_a_table_made_again_twice_is_rolled_back() {
    run_scripts(&[
        ScriptTest {
            name: "a rollback of a transaction or of savepoints that made a table again twice leaves the old table's rows numbered on after its last",
            set_up_script: &[
                "CREATE TABLE t2 (id INT PRIMARY KEY);",
                "INSERT INTO t2 VALUES (1), (2), (3), (4), (5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t2",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id INT PRIMARY KEY)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (10), (11), (12)",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t2",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id INT PRIMARY KEY)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (6)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, id FROM t2 ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,3)"), T("3")],
                            &[T("(0,4)"), T("4")],
                            &[T("(0,5)"), T("5")],
                            &[T("(0,6)"), T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT a",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t2",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id INT PRIMARY KEY)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (10), (11), (12)",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT b",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t2",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (id INT PRIMARY KEY)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT a",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (7)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, id FROM t2 ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("id", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,3)"), T("3")],
                            &[T("(0,4)"), T("4")],
                            &[T("(0,5)"), T("5")],
                            &[T("(0,6)"), T("6")],
                            &[T("(0,7)"), T("7")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_copies_of_an_equal_row_have_ctids_of_their_own() {
    run_scripts(&[
        ScriptTest {
            name: "each copy of an equal row of a table without a primary key has its own ctid and xmin",
            set_up_script: &[
                "CREATE TABLE dup (a INT, b TEXT);",
                "INSERT INTO dup VALUES (1, 'x'), (1, 'x'), (1, 'x'), (2, 'y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b FROM dup ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("x")],
                            &[T("(0,2)"), T("1"), T("x")],
                            &[T("(0,3)"), T("1"), T("x")],
                            &[T("(0,4)"), T("2"), T("y")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a FROM dup WHERE ctid = '(0,2)';",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,2)"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a FROM dup WHERE ctid >= '(0,2)' AND ctid < '(0,4)' ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,2)"), T("1")],
                            &[T("(0,3)"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dup SET b = 'z' WHERE ctid = '(0,2)' RETURNING ctid, a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,5)"), T("1"), T("z")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dup WHERE ctid = '(0,1)' RETURNING ctid, a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("x")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b FROM dup ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,3)"), T("1"), T("x")],
                            &[T("(0,4)"), T("2"), T("y")],
                            &[T("(0,5)"), T("1"), T("z")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE seen AS SELECT ctid AS t, xmin::TEXT AS x FROM dup;",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dup VALUES (1, 'x'), (2, 'y');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dup WHERE ctid = '(0,3)';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.ctid, d.xmin::TEXT = s.x AS same FROM dup d JOIN seen s ON d.ctid = s.t ORDER BY d.ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("same", BOOL)],
                        rows: &[
                            &[T("(0,4)"), T("t")],
                            &[T("(0,5)"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dup a USING dup b WHERE a.a = b.a AND a.b = b.b AND a.ctid < b.ctid;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b FROM dup ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,5)"), T("1"), T("z")],
                            &[T("(0,6)"), T("1"), T("x")],
                            &[T("(0,7)"), T("2"), T("y")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "writers of different copies of an equal row never wait and a writer of one copy waits for its other writer",
            set_up_script: &[
                "CREATE TABLE c2 (a INT);",
                "INSERT INTO c2 VALUES (1), (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM c2 WHERE ctid = '(0,1)'",
                    expected: Expected::Tag("DELETE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM c2 WHERE ctid = '(0,2)'",
                    expected: Expected::Tag("DELETE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DELETE FROM c2 WHERE ctid = '(0,1)'",
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
                    query: "/* client B */ SELECT count(*) FROM c2",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
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
fn test_where_current_of_changes_the_copy_its_cursor_is_on() {
    run_scripts(&[
        ScriptTest {
            name: "DELETE WHERE CURRENT OF removes the copy of an equal row its cursor is on, and the other copies keep their ctid and xmin",
            set_up_script: &[
                "CREATE TABLE cur (a INT, b TEXT);",
                "INSERT INTO cur VALUES (1, 'x'), (1, 'x'), (1, 'x');",
                "CREATE TABLE kept AS SELECT ctid AS t, xmin::TEXT AS x FROM cur;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT ctid, a, b FROM cur;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("x")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,2)"), T("1"), T("x")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cur WHERE CURRENT OF c RETURNING ctid, a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,2)"), T("1"), T("x")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.ctid, c.xmin::TEXT = k.x AS same FROM cur c JOIN kept k ON c.ctid = k.t ORDER BY c.ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("same", BOOL)],
                        rows: &[
                            &[T("(0,1)"), T("t")],
                            &[T("(0,3)"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "UPDATE WHERE CURRENT OF changes the copy of an equal row its cursor is on, and the other copies keep their ctid and xmin",
            set_up_script: &[
                "CREATE TABLE cur (a INT, b TEXT);",
                "INSERT INTO cur VALUES (1, 'x'), (1, 'x'), (1, 'x');",
                "CREATE TABLE kept AS SELECT ctid AS t, xmin::TEXT AS x FROM cur;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT ctid, a, b FROM cur;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("x")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,2)"), T("1"), T("x")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cur SET b = 'y' WHERE CURRENT OF c RETURNING ctid, a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,4)"), T("1"), T("y")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b FROM cur ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("x")],
                            &[T("(0,3)"), T("1"), T("x")],
                            &[T("(0,4)"), T("1"), T("y")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.ctid, c.xmin::TEXT = k.x AS same FROM cur c JOIN kept k ON c.ctid = k.t ORDER BY c.ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("same", BOOL)],
                        rows: &[
                            &[T("(0,1)"), T("t")],
                            &[T("(0,3)"), T("t")],
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
fn test_ctids_packed_as_postgres_heap_pages() {
    run_scripts(&[
        ScriptTest {
            name: "ctids fill blocks as Postgres' heap fills pages, by each row's heap tuple length with its line pointer, the fill factor's reserve, NULL bitmaps, values moved out of line, and a page per row too wide to share",
            set_up_script: &[
                "CREATE TABLE ct_int (a int);",
                "INSERT INTO ct_int SELECT g FROM generate_series(1, 1000) g;",
                "CREATE TABLE ct_ff (a int, b text) WITH (fillfactor = 50);",
                "INSERT INTO ct_ff SELECT g, repeat('x', 100) FROM generate_series(1, 300) g;",
                "CREATE TABLE ct_nulls (a int, b int, c int, d int, e int, f int, g int, h int, i int);",
                "INSERT INTO ct_nulls SELECT g, NULL, g, NULL, g, NULL, g, NULL, g FROM generate_series(1, 500) g;",
                "CREATE TABLE ct_wide (a int, b text);",
                "INSERT INTO ct_wide SELECT g, repeat('y', 1500) FROM generate_series(1, 20) g;",
                "CREATE TABLE ct_toast (a int, b text);",
                "ALTER TABLE ct_toast ALTER COLUMN b SET STORAGE EXTERNAL;",
                "INSERT INTO ct_toast SELECT g, repeat('z', 5000) FROM generate_series(1, 300) g;",
                "CREATE TABLE ct_upd (a int);",
                "INSERT INTO ct_upd VALUES (1), (2), (3);",
                "CREATE TABLE ct_brin (a int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT (ctid::text::point)[0] AS block, count(*), min(ctid), max(ctid) FROM ct_int GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("block", FLOAT8), Column("count", INT8), Column("min", TID), Column("max", TID)],
                        rows: &[
                            &[T("0"), T("226"), T("(0,1)"), T("(0,226)")],
                            &[T("1"), T("226"), T("(1,1)"), T("(1,226)")],
                            &[T("2"), T("226"), T("(2,1)"), T("(2,226)")],
                            &[T("3"), T("226"), T("(3,1)"), T("(3,226)")],
                            &[T("4"), T("96"), T("(4,1)"), T("(4,96)")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ctid::text::point)[0] AS block, count(*) FROM ct_ff GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("block", FLOAT8), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("29")],
                            &[T("1"), T("29")],
                            &[T("2"), T("29")],
                            &[T("3"), T("29")],
                            &[T("4"), T("29")],
                            &[T("5"), T("29")],
                            &[T("6"), T("29")],
                            &[T("7"), T("29")],
                            &[T("8"), T("29")],
                            &[T("9"), T("29")],
                            &[T("10"), T("10")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ctid::text::point)[0] AS block, count(*) FROM ct_nulls GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("block", FLOAT8), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("136")],
                            &[T("1"), T("136")],
                            &[T("2"), T("136")],
                            &[T("3"), T("92")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ctid::text::point)[0] AS block, count(*) FROM ct_wide GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("block", FLOAT8), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("5")],
                            &[T("1"), T("5")],
                            &[T("2"), T("5")],
                            &[T("3"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ctid::text::point)[0] AS block, count(*) FROM ct_toast GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("block", FLOAT8), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("157")],
                            &[T("1"), T("143")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ct_upd SET a = a + 10 WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a FROM ct_upd ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,3)"), T("3")],
                            &[T("(0,4)"), T("12")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE ct_upd;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ct_upd VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a FROM ct_upd ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE curtid tid; BEGIN LOOP INSERT INTO ct_brin VALUES (1) RETURNING ctid INTO curtid; EXIT WHEN curtid > tid '(2, 0)'; END LOOP; END; $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), max(ctid) FROM ct_brin;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("max", TID)],
                        rows: &[
                            &[T("453"), T("(2,1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ct_comp (a int, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ct_comp ALTER COLUMN b SET STORAGE MAIN;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ct_comp SELECT g, repeat('x', 3000) FROM generate_series(1, 20) g;",
                    expected: Expected::Tag("INSERT 0 20"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (ctid::text::point)[0] AS block, count(*) FROM ct_comp GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("block", FLOAT8), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("a row's width counts its values uncompressed, where Postgres compresses wide values in line (Appendix C)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_tid_scans_read_rows_in_ctid_order() {
    run_scripts(&[
        ScriptTest {
            name: "TID scans and TID range scans return rows in ctid order in either direction",
            set_up_script: &[
                "CREATE TABLE tord (id INT PRIMARY KEY, name TEXT);",
                "INSERT INTO tord VALUES (5, 'a'), (3, 'b'), (9, 'c'), (1, 'd'), (7, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT id, ctid FROM tord WHERE ctid >= '(0,2)' AND ctid <= '(0,4)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ctid", TID)],
                        rows: &[
                            &[T("3"), T("(0,2)")],
                            &[T("9"), T("(0,3)")],
                            &[T("1"), T("(0,4)")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id FROM tord WHERE ctid = ANY ('{"(0,4)","(0,1)"}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE tord SET name = 'z' WHERE id = 3;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM tord WHERE id = 9;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id, ctid FROM tord WHERE ctid >= '(0,1)' AND ctid < '(1,0)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ctid", TID)],
                        rows: &[
                            &[T("5"), T("(0,1)")],
                            &[T("1"), T("(0,4)")],
                            &[T("7"), T("(0,5)")],
                            &[T("3"), T("(0,6)")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DECLARE c SCROLL CURSOR FOR SELECT id, ctid FROM tord WHERE ctid < '(1,0)';"#,
                    expected: Expected::Tag("DECLARE CURSOR"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"FETCH 2 FROM c;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ctid", TID)],
                        rows: &[
                            &[T("5"), T("(0,1)")],
                            &[T("1"), T("(0,4)")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"FETCH BACKWARD 1 FROM c;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ctid", TID)],
                        rows: &[
                            &[T("5"), T("(0,1)")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"FETCH FORWARD 3 FROM c;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ctid", TID)],
                        rows: &[
                            &[T("1"), T("(0,4)")],
                            &[T("7"), T("(0,5)")],
                            &[T("3"), T("(0,6)")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"FETCH BACKWARD ALL FROM c;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("ctid", TID)],
                        rows: &[
                            &[T("7"), T("(0,5)")],
                            &[T("1"), T("(0,4)")],
                            &[T("5"), T("(0,1)")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMIT;"#,
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a ctid names one copy of an equal row of a table without a primary key, and a ctid range crosses them",
            set_up_script: &[
                "CREATE TABLE kdup (a INT, b TEXT);",
                "INSERT INTO kdup VALUES (1, 'x'), (2, 'y'), (1, 'x'), (1, 'x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a, b FROM kdup WHERE ctid = '(0,3)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,3)"), T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a, b FROM kdup WHERE ctid >= '(0,3)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,3)"), T("1"), T("x")],
                            &[T("(0,4)"), T("1"), T("x")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a, b FROM kdup WHERE ctid > '(0,1)' AND ctid < '(0,4)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,2)"), T("2"), T("y")],
                            &[T("(0,3)"), T("1"), T("x")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM kdup WHERE ctid = '(0,3)';"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a, b FROM kdup WHERE ctid >= '(0,1)' AND ctid < '(1,0)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("x")],
                            &[T("(0,2)"), T("2"), T("y")],
                            &[T("(0,4)"), T("1"), T("x")],
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
fn test_currtid2() {
    run_scripts(&[
        ScriptTest {
            name: "currtid2 checks the relation it names, as Postgres' tid.sql runs it",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE tid_tab (a INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE MATERIALIZED VIEW tid_matview AS SELECT a FROM tid_tab;"#,
                    expected: Expected::Tag("SELECT 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_matview'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"tid (0, 1) is not valid for relation "tid_matview""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO tid_tab VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"REFRESH MATERIALIZED VIEW tid_matview;"#,
                    expected: Expected::Tag("REFRESH MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_matview'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Rows {
                        columns: &[Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"TRUNCATE tid_tab;"#,
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE SEQUENCE tid_seq;"#,
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_seq'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Rows {
                        columns: &[Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE INDEX tid_ind ON tid_tab (a);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_ind'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"cannot open relation "tid_ind""#, detail: "This operation is not supported for indexes.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE tid_part (a INT) PARTITION BY RANGE (a);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_part'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot look at latest visible tid for relation "public.tid_part""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE VIEW tid_view_no_ctid AS SELECT a FROM tid_tab;"#,
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_view_no_ctid'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"currtid cannot handle views with no CTID"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE VIEW tid_view_with_ctid AS SELECT ctid, a FROM tid_tab;"#,
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_view_with_ctid'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"tid (0, 1) is not valid for relation "tid_tab""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO tid_tab VALUES (1);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_view_with_ctid'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Rows {
                        columns: &[Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE VIEW tid_view_fake_ctid AS SELECT 1 AS ctid, 2 AS a;"#,
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('tid_view_fake_ctid'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"ctid isn't of type TID"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('no_such_table'::TEXT, '(0,1)'::TID);"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "no_such_table" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "currtid2 follows a row's updates to its newest version a snapshot taken now sees",
            set_up_script: &[
                "CREATE TABLE ct (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"UPDATE ct SET v = 'b';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, currtid2('ct', '(0,1)') FROM ct;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,2)"), T("(0,2)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE ct SET id = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, currtid2('ct', '(0,1)'), currtid2('ct', '(0,2)') FROM ct;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("currtid2", TID), Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,3)"), T("(0,3)"), T("(0,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('ct', '(0,9)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,9)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('ct', '(5,1)');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"tid (5, 1) is not valid for relation "ct""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('ct', '(0,0)');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"tid (0, 0) is not valid for relation "ct""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE ct SET v = 'c';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, currtid2('ct', '(0,1)') FROM ct;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,4)"), T("(0,4)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('ct', '(0,1)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM ct;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('ct', '(0,1)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE kc (a INT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO kc VALUES (1), (1);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE kc SET a = 2 WHERE ctid = '(0,1)';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a, currtid2('kc', '(0,1)'), currtid2('kc', '(0,2)') FROM kc ORDER BY ctid;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("currtid2", TID), Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,2)"), T("1"), T("(0,3)"), T("(0,2)")],
                            &[T("(0,3)"), T("2"), T("(0,3)"), T("(0,2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE dk (id INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO dk VALUES (1, 'a');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE dk SET v = 'b';"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, currtid2('dk', '(0,1)') FROM dk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,2)"), T("(0,2)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE ROLE currtid_reader;"#,
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET ROLE currtid_reader;"#,
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT currtid2('ct', '(0,1)');"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied for table ct"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"RESET ROLE;"#,
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "currtid2 in a serializable transaction reads past a running update as a conflict out, as a read does",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO t VALUES (1, 'a'), (2, 'b');",
                "CREATE TABLE s (a INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"/* client A */ BEGIN ISOLATION LEVEL SERIALIZABLE;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"/* client A */ SELECT count(*) FROM s;"#,
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
                    query: r#"/* client B */ BEGIN ISOLATION LEVEL SERIALIZABLE;"#,
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"/* client B */ SELECT v FROM t WHERE id = 2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[
                            &[T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"/* client B */ UPDATE t SET v = 'x' WHERE id = 1;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"/* client A */ SELECT currtid2('t', '(0,1)');"#,
                    expected: Expected::Rows {
                        columns: &[Column("currtid2", TID)],
                        rows: &[
                            &[T("(0,1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"/* client A */ UPDATE t SET v = 'y' WHERE id = 2;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"/* client B */ COMMIT;"#,
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"/* client A */ COMMIT;"#,
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to read/write dependencies among transactions", detail: "Reason code: Canceled on identification as a pivot, during commit attempt.", hint: "The transaction might succeed if retried.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_updates_of_rows_their_transaction_locked() {
    run_scripts(&[
        ScriptTest {
            name: "a row a transaction locked and then updated keeps the lock in its new version's xmax",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE lk (id INT PRIMARY KEY, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lk VALUES (1, 0), (2, 0);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lkk (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lkk VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lkn (id INT PRIMARY KEY, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lkn VALUES (1, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT txid_current() > 0 AS has_xid;",
                    expected: Expected::Rows {
                        columns: &[Column("has_xid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM lk WHERE id = 1 FOR UPDATE;",
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
                    query: "UPDATE lk SET v = 1 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, xmax = 0 AS xmax_zero, xmax::text = (txid_current() % 4294967296)::text AS xmax_mine FROM lk ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("xmax_zero", BOOL), Column("xmax_mine", BOOL)],
                        rows: &[
                            &[T("1"), T("1"), T("f"), T("t")],
                            &[T("2"), T("0"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM lk WHERE id = 2 FOR UPDATE;",
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
                    query: "UPDATE lk SET v = 2 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, xmax = 0 AS xmax_zero FROM lk ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("xmax_zero", BOOL)],
                        rows: &[
                            &[T("1"), T("1"), T("f")],
                            &[T("2"), T("2"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE s;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM lkk FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE lkk SET a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, xmax = 0 AS xmax_zero FROM lkk;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("xmax_zero", BOOL)],
                        rows: &[
                            &[T("2"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM lkn FOR SHARE;",
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
                    query: "UPDATE lkn SET id = 5;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, xmax = 0 AS xmax_zero FROM lkn;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("xmax_zero", BOOL)],
                        rows: &[
                            &[T("5"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE lkn SET v = 9;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, xmax = 0 AS xmax_zero FROM lkn;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("xmax_zero", BOOL)],
                        rows: &[
                            &[T("5"), T("9"), T("f")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "an update of a locked row rolled back before it wrote the row's new version leaves no lock behind",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE lkr (id INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lkr VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM lkr WHERE id = 1 FOR UPDATE;",
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
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE lkr SET id = 2 WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "lkr_pkey""#, detail: "Key (id)=(2) already exists.", schema: "public", table: "lkr", constraint: "lkr_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM lkr WHERE id = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lkr VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, xmax = 0 AS xmax_zero FROM lkr ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("xmax_zero", BOOL)],
                        rows: &[
                            &[T("1"), T("f")],
                            &[T("2"), T("t")],
                        ],
                        tag: "SELECT 2",
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
