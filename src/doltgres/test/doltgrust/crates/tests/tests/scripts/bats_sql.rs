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

//! Dolt's integration-tests/bats/sql.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql() {
    run_scripts(&[
        ScriptTest {
            name: "sql: errors do not write incomplete rows",
            intent: "A statement that fails partway through writes none of its rows.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint PRIMARY KEY, v1 bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON test (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1,1), (4,4), (5,5);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2,2), (3,3), (1,1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET pk = pk + 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(5) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk bigint PRIMARY KEY, CONSTRAINT fk_test FOREIGN KEY (pk) REFERENCES test (v1));",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "there is no unique constraint matching given keys for referenced table \"test\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test2 VALUES (4);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 13, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE pk > 0;",
                    expected: Expected::Tag("DELETE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1,7), (4,8), (5,9) ON CONFLICT (\"pk\") DO UPDATE SET \"v1\" = EXCLUDED.\"v1\";",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("7")],
                            &[T("4"), T("8")],
                            &[T("5"), T("9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test2\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: select from multiple tables",
            intent: "A query reads columns from two tables at once.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2 FROM one_pk,two_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("1")],
                            &[T("0"), T("1"), T("0")],
                            &[T("0"), T("1"), T("1")],
                            &[T("1"), T("0"), T("0")],
                            &[T("1"), T("0"), T("1")],
                            &[T("1"), T("1"), T("0")],
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("0"), T("0")],
                            &[T("2"), T("0"), T("1")],
                            &[T("2"), T("1"), T("0")],
                            &[T("2"), T("1"), T("1")],
                            &[T("3"), T("0"), T("0")],
                            &[T("3"), T("0"), T("1")],
                            &[T("3"), T("1"), T("0")],
                            &[T("3"), T("1"), T("1")],
                        ],
                        tag: "SELECT 16",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2 FROM one_pk,two_pk WHERE one_pk.c1=two_pk.c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("0"), T("1")],
                            &[T("2"), T("1"), T("0")],
                            &[T("3"), T("1"), T("1")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2,one_pk.c1 AS foo,two_pk.c1 AS bar FROM one_pk,two_pk WHERE one_pk.c1=two_pk.c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8), Column("foo", INT8), Column("bar", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0")],
                            &[T("1"), T("0"), T("1"), T("10"), T("10")],
                            &[T("2"), T("1"), T("0"), T("20"), T("20")],
                            &[T("3"), T("1"), T("1"), T("30"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT n.nspname::text AS table_schema, c.relname::text AS table_name, CASE WHEN i.indisprimary THEN 'PRIMARY' ELSE ic.relname::text END AS index_name, (NOT i.indisunique)::int AS non_unique, k.n::int AS seq_in_index, a.attname::text AS column_name FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_class ic ON ic.oid = i.indexrelid CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY AS k(attnum, n) JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = k.attnum WHERE n.nspname = 'public') AS statistics;",
                    expected: Expected::Rows {
                        columns: &[Column("table_schema", TEXT), Column("table_name", TEXT), Column("index_name", TEXT), Column("non_unique", INT4), Column("seq_in_index", INT4), Column("column_name", TEXT)],
                        rows: &[
                            &[T("public"), T("one_pk"), T("PRIMARY"), T("0"), T("1"), T("pk")],
                            &[T("public"), T("two_pk"), T("PRIMARY"), T("0"), T("1"), T("pk1")],
                            &[T("public"), T("two_pk"), T("PRIMARY"), T("0"), T("2"), T("pk2")],
                            &[T("public"), T("has_datetimes"), T("PRIMARY"), T("0"), T("1"), T("pk")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("information_schema.STATISTICS is MySQL's, so pg_index lists each index's columns, and its cardinality column, MySQL's estimate of an index's distinct values, has no Postgres counterpart")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: ambiguous column name",
            intent: "A column name found in two tables of the query is refused as ambiguous.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2 FROM one_pk,two_pk WHERE c1=0;",
                    expected: Expected::Error(Diagnostic { code: "42702", message: "column reference \"c1\" is ambiguous", position: 44, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: select with and and or clauses",
            intent: "WHERE combines AND and OR conditions.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2 FROM one_pk,two_pk WHERE pk=0 AND pk1=0 OR pk2=1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("1")],
                            &[T("0"), T("1"), T("1")],
                            &[T("1"), T("0"), T("1")],
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("0"), T("1")],
                            &[T("2"), T("1"), T("1")],
                            &[T("3"), T("0"), T("1")],
                            &[T("3"), T("1"), T("1")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: select the same column twice using column aliases",
            intent: "One column can be selected twice under different aliases.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk,c1 AS foo,c1 AS bar FROM one_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("foo", INT8), Column("bar", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("10"), T("10")],
                            &[T("2"), T("20"), T("20")],
                            &[T("3"), T("30"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: select same column twice using table aliases",
            intent: "One table joined to itself under two aliases selects the same column twice.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT foo.pk,foo.c1,bar.c1 FROM one_pk AS foo, one_pk AS bar;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("10")],
                            &[T("0"), T("0"), T("20")],
                            &[T("0"), T("0"), T("30")],
                            &[T("1"), T("10"), T("0")],
                            &[T("1"), T("10"), T("10")],
                            &[T("1"), T("10"), T("20")],
                            &[T("1"), T("10"), T("30")],
                            &[T("2"), T("20"), T("0")],
                            &[T("2"), T("20"), T("10")],
                            &[T("2"), T("20"), T("20")],
                            &[T("2"), T("20"), T("30")],
                            &[T("3"), T("30"), T("0")],
                            &[T("3"), T("30"), T("10")],
                            &[T("3"), T("30"), T("20")],
                            &[T("3"), T("30"), T("30")],
                        ],
                        tag: "SELECT 16",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: select ambiguous column using table aliases",
            intent: "A column that both aliases of a self-join have is refused as ambiguous without an alias.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk,foo.c1,bar.c1 FROM one_pk AS foo, one_pk AS bar;",
                    expected: Expected::Error(Diagnostic { code: "42702", message: "column reference \"pk\" is ambiguous", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: basic inner join",
            intent: "An inner join returns the matching rows of both tables.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2 FROM one_pk JOIN two_pk ON one_pk.c1=two_pk.c1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("0"), T("1")],
                            &[T("2"), T("1"), T("0")],
                            &[T("3"), T("1"), T("1")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2 FROM two_pk JOIN one_pk ON one_pk.c1=two_pk.c1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("0"), T("1")],
                            &[T("2"), T("1"), T("0")],
                            &[T("3"), T("1"), T("1")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2 FROM one_pk JOIN two_pk ON one_pk.c1=two_pk.c1 WHERE pk=1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2,one_pk.c1 AS foo,two_pk.c1 AS bar FROM one_pk JOIN two_pk ON one_pk.c1=two_pk.c1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8), Column("foo", INT8), Column("bar", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0")],
                            &[T("1"), T("0"), T("1"), T("10"), T("10")],
                            &[T("2"), T("1"), T("0"), T("20"), T("20")],
                            &[T("3"), T("1"), T("1"), T("30"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,pk1,pk2,one_pk.c1 AS foo,two_pk.c1 AS bar FROM one_pk JOIN two_pk ON one_pk.c1=two_pk.c1 WHERE one_pk.c1=10 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8), Column("foo", INT8), Column("bar", INT8)],
                        rows: &[
                            &[T("1"), T("0"), T("1"), T("10"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: select two tables and join to one",
            intent: "Two tables join to a third in one query.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT op.pk,pk1,pk2 FROM one_pk,two_pk JOIN one_pk AS op ON op.pk=pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("pk1", INT8), Column("pk2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("1")],
                            &[T("1"), T("1"), T("0")],
                            &[T("1"), T("1"), T("1")],
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("1")],
                            &[T("1"), T("1"), T("0")],
                            &[T("1"), T("1"), T("1")],
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("1")],
                            &[T("1"), T("1"), T("0")],
                            &[T("1"), T("1"), T("1")],
                            &[T("0"), T("0"), T("0")],
                            &[T("0"), T("0"), T("1")],
                            &[T("1"), T("1"), T("0")],
                            &[T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 16",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: non unique table alias",
            intent: "A table named twice in FROM without aliases is refused.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk,one_pk;",
                    expected: Expected::Error(Diagnostic { code: "42712", message: "table name \"one_pk\" specified more than once", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: is null and is not null statements",
            intent: "IS NULL and IS NOT NULL select rows by missing values.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO one_pk (pk,c1,c2) VALUES (11,0,0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c3 IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c3 IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: addition and subtraction",
            intent: "Arithmetic works in the select list and WHERE.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (11,0,5,10,15,20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c2-c1>=5;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c3-c2-c1>=5;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c2+c1<=5;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("0")],
                            &[T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: order by and limit",
            intent: "ORDER BY sorts rows and LIMIT cuts them off.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk LIMIT 1 OFFSET 0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk LIMIT 1 OFFSET 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("1"), T("10"), T("10"), T("10"), T("10"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk LIMIT 0 OFFSET 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("3"), T("30"), T("30"), T("30"), T("30"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM two_pk ORDER BY pk1, pk2 DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("10"), T("10"), T("10"), T("10"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c2 FROM one_pk ORDER BY c1 LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk,two_pk ORDER BY pk1,pk2,pk LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8), Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk CROSS JOIN two_pk ORDER BY pk1,pk2,pk LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8), Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's JOIN without ON is a CROSS JOIN")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY LIMIT 1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"LIMIT\"", position: 31, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY bad LIMIT 1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"bad\" does not exist", position: 31, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER pk BY LIMIT;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"pk\"", position: 28, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: limit less than zero",
            intent: "A negative LIMIT is refused.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk LIMIT -1;",
                    expected: Expected::Error(Diagnostic { code: "2201W", message: "LIMIT must not be negative", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk LIMIT -2;",
                    expected: Expected::Error(Diagnostic { code: "2201W", message: "LIMIT must not be negative", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk LIMIT -1,1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "LIMIT #,# syntax is not supported", hint: "Use separate LIMIT and OFFSET clauses.", position: 34, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: addition on both left and right sides of comparison operator",
            intent: "Arithmetic works on both sides of a comparison.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (11,5,5,10,15,20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c2+c1<=5+5;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("0")],
                            &[T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: select with in list",
            intent: "IN and NOT IN select rows by a list of values.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c1 IN (10,20);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c1 IN (11,21);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c1 NOT IN (10,20);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
                        rows: &[
                            &[T("0")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c1 NOT IN (10,20) AND c1 IN (30);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8)],
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
        ScriptTest {
            name: "sql: parser does not support empty list",
            intent: "An empty IN list is a syntax error.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pk FROM one_pk WHERE c1 NOT IN ();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \")\"", position: 40, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: addition in join statement",
            intent: "A join condition can use arithmetic.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk JOIN two_pk ON pk1-pk>0 AND pk2<1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8), Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), T("0"), T("0"), T("0"), T("1"), T("0"), T("20"), T("20"), T("20"), T("20"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: leave off table name in select",
            intent: "A column named without a FROM clause is refused.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO one_pk (pk,c1,c2) VALUES (11,0,0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk WHERE c3 IS NULL;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"pk\" does not exist", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: show tables",
            intent: "The catalog lists the tables of the current schema.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("has_datetimes")],
                            &[T("one_pk")],
                            &[T("two_pk")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: show tables AS OF",
            intent: "The catalog read AS OF an older commit lists only the tables that commit had.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit tables');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE table_a(x int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE table_b(x int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit tables');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("has_datetimes")],
                            &[T("one_pk")],
                            &[T("table_a")],
                            &[T("table_b")],
                            &[T("two_pk")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables AS OF 'HEAD~' WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("has_datetimes")],
                            &[T("one_pk")],
                            &[T("two_pk")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: describe",
            intent: "The catalog describes a table's columns, types, and nullability.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'one_pk' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("c1"), T("bigint"), T("YES")],
                            &[T("c2"), T("bigint"), T("YES")],
                            &[T("c3"), T("bigint"), T("YES")],
                            &[T("c4"), T("bigint"), T("YES")],
                            &[T("c5"), T("bigint"), T("YES")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so information_schema.columns describes the table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: describe bad table name",
            intent: "Describing a missing table finds no columns.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'poop' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so information_schema.columns describes the table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: alter table to add and delete a column",
            intent: "A column can be added to a table and dropped again.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk ADD COLUMN c6 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ADD (column) is ADD COLUMN")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'one_pk' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("c1"), T("bigint"), T("YES")],
                            &[T("c2"), T("bigint"), T("YES")],
                            &[T("c3"), T("bigint"), T("YES")],
                            &[T("c4"), T("bigint"), T("YES")],
                            &[T("c5"), T("bigint"), T("YES")],
                            &[T("c6"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so information_schema.columns describes the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk DROP COLUMN c6;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'one_pk' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("c1"), T("bigint"), T("YES")],
                            &[T("c2"), T("bigint"), T("YES")],
                            &[T("c3"), T("bigint"), T("YES")],
                            &[T("c4"), T("bigint"), T("YES")],
                            &[T("c5"), T("bigint"), T("YES")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so information_schema.columns describes the table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: alter table to rename a column",
            intent: "A renamed column keeps its place and type under the new name.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk ADD COLUMN c6 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ADD (column) is ADD COLUMN")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk RENAME COLUMN c6 TO c7;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'one_pk' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("c1"), T("bigint"), T("YES")],
                            &[T("c2"), T("bigint"), T("YES")],
                            &[T("c3"), T("bigint"), T("YES")],
                            &[T("c4"), T("bigint"), T("YES")],
                            &[T("c5"), T("bigint"), T("YES")],
                            &[T("c7"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so information_schema.columns describes the table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: alter table change column to rename a column",
            intent: "A renamed column changed to its own type stays under the new name.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk ADD COLUMN c6 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ADD (column) is ADD COLUMN")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk RENAME COLUMN c6 TO c7;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk ALTER COLUMN c7 DROP DEFAULT, ALTER COLUMN c7 TYPE int USING c7::int, ALTER COLUMN c7 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'one_pk' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("c1"), T("bigint"), T("YES")],
                            &[T("c2"), T("bigint"), T("YES")],
                            &[T("c3"), T("bigint"), T("YES")],
                            &[T("c4"), T("bigint"), T("YES")],
                            &[T("c5"), T("bigint"), T("YES")],
                            &[T("c7"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so information_schema.columns describes the table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: alter table without parentheses",
            intent: "ALTER TABLE adds a column written without parentheses.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk ADD c6 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'one_pk' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("c1"), T("bigint"), T("YES")],
                            &[T("c2"), T("bigint"), T("YES")],
                            &[T("c3"), T("bigint"), T("YES")],
                            &[T("c4"), T("bigint"), T("YES")],
                            &[T("c5"), T("bigint"), T("YES")],
                            &[T("c6"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's, so information_schema.columns describes the table")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: alter table modify column with no actual change",
            intent: "Changing a column to the type it already has changes nothing.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk ALTER COLUMN c5 DROP DEFAULT, ALTER COLUMN c5 TYPE bigint USING c5::bigint, ALTER COLUMN c5 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: alter table change column with no actual change",
            intent: "Renaming a column to its own name with its own type changes nothing.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE one_pk ALTER COLUMN c5 DROP DEFAULT, ALTER COLUMN c5 TYPE bigint USING c5::bigint, ALTER COLUMN c5 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: alter table modify column type success",
            intent: "A column's type changes when every value converts.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1(pk bigint PRIMARY KEY, v1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON t1 (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2(pk bigint PRIMARY KEY, v1 VARCHAR(20));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON t2 (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3(pk bigint PRIMARY KEY, v1 timestamp);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON t3 (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (0,-1),(1,1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (0,'hi'),(1,'bye');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3 VALUES (0,'1999-11-02 17:39:38'),(1,'2021-01-08 02:59:27');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN v1 DROP DEFAULT, ALTER COLUMN v1 TYPE bigint USING v1::bigint, ALTER COLUMN v1 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ALTER COLUMN v1 DROP DEFAULT, ALTER COLUMN v1 TYPE VARCHAR(2000) USING v1::VARCHAR(2000), ALTER COLUMN v1 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t3 ALTER COLUMN v1 DROP DEFAULT, ALTER COLUMN v1 TYPE TIMESTAMP USING v1::TIMESTAMP, ALTER COLUMN v1 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("0"), T("-1")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("hi")],
                            &[T("1"), T("bye")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", TIMESTAMP)],
                        rows: &[
                            &[T("0"), T("1999-11-02 17:39:38")],
                            &[T("1"), T("2021-01-08 02:59:27")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t4(pk bigint PRIMARY KEY, v1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON t4 (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t4 VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t4 ALTER COLUMN pk DROP DEFAULT, ALTER COLUMN pk TYPE float USING pk::float, ALTER COLUMN pk SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t4 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", FLOAT8), Column("v1", INT4)],
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
            name: "sql: alter table modify column type failure",
            intent: "A column's type change fails when a value doesn't fit the new type.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1(pk bigint PRIMARY KEY, v1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON t1 (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (0,-1),(1,1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN v1 DROP DEFAULT, ALTER COLUMN v1 TYPE bigint USING v1::bigint, ALTER COLUMN v1 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: alter table modify column type no data change",
            intent: "Widening an indexed column's type keeps its rows and leaves no row diff.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1(pk bigint PRIMARY KEY, v1 VARCHAR(64));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX  ON t1 (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (0,NULL),(1,NULL);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN v1 DROP DEFAULT, ALTER COLUMN v1 TYPE VARCHAR(100) USING v1::VARCHAR(100), ALTER COLUMN v1 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: drop table",
            intent: "A dropped table is gone and can't be queried.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE one_pk;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE poop;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "table \"poop\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: replace count",
            intent: "An upsert on an existing key replaces the row's values.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1) ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2) ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT8)],
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
        ScriptTest {
            name: "sql: select union all",
            intent: "UNION ALL keeps duplicate rows.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 2+2  UNION ALL SELECT 2+2  UNION ALL SELECT 2+3 ;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: select union",
            intent: "UNION removes duplicate rows.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 2+2  UNION SELECT 2+2  UNION SELECT 2+3 ;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 2+2  UNION DISTINCT SELECT 2+2  UNION SELECT 2+3 ;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "(SELECT 2+2  UNION DISTINCT SELECT 2+2 ) UNION SELECT 2+3 ;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 2+2  UNION DISTINCT (SELECT 2+2  UNION SELECT 2+3 );",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: greatest/least with a timestamp",
            intent: "GREATEST and LEAST compare timestamps.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT GREATEST(NOW(), NOW() + INTERVAL '2 days') ;",
                    expected: Expected::Rows {
                        columns: &[Column("greatest", TIMESTAMPTZ)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's DATE_ADD and DATE_SUB are + and - of an interval")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT LEAST(NOW(), NOW() + INTERVAL '2 days') ;",
                    expected: Expected::Rows {
                        columns: &[Column("least", TIMESTAMPTZ)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's DATE_ADD and DATE_SUB are + and - of an interval")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: greatest with converted null",
            intent: "GREATEST ignores a NULL among converted values.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT GREATEST(CAST(NOW() AS text), CAST(NULL AS text)) ;",
                    expected: Expected::Rows {
                        columns: &[Column("greatest", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: DATE_ADD and DATE_SUB in where clause",
            intent: "Date arithmetic works inside WHERE.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM has_datetimes WHERE date_created > '2020-02-18 00:00:00'::timestamp - INTERVAL '2 days';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("date_created", TIMESTAMP)],
                        rows: &[
                            &[T("0"), T("2020-02-17 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's DATE_ADD and DATE_SUB are + and - of an interval")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM has_datetimes WHERE date_created > '2020-02-14 00:00:00'::timestamp + INTERVAL '2 days';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("date_created", TIMESTAMP)],
                        rows: &[
                            &[T("0"), T("2020-02-17 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's DATE_ADD and DATE_SUB are + and - of an interval")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: update a datetime column",
            intent: "A timestamp column can be updated.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO has_datetimes (pk) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE has_datetimes SET date_created='2020-02-11 00:00:00' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: group by statements",
            intent: "GROUP BY groups rows for aggregates.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (4,0,0,0,0,0),(5,0,0,0,0,0);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT max(pk) FROM one_pk GROUP BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT8)],
                        rows: &[
                            &[T("3")],
                            &[T("5")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT max(pk), min(c2) FROM one_pk GROUP BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT8), Column("min", INT8)],
                        rows: &[
                            &[T("3"), T("30")],
                            &[T("5"), T("0")],
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT max(pk),c2 FROM one_pk GROUP BY c2;",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T("3"), T("30")],
                            &[T("5"), T("0")],
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: substr() and cast() functions",
            intent: "substr() and casts work in queries.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT substr(CAST(date_created AS text), 1, 4) FROM has_datetimes;",
                    expected: Expected::Rows {
                        columns: &[Column("substr", TEXT)],
                        rows: &[
                            &[T("2020")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: divide by zero does not panic",
            intent: "Dividing by zero is an error, not a crash.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 1/0 ;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    dolt: Some(Dolt::Differs("Postgres raises division_by_zero, where MySQL returns NULL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1.0/0.0 ;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    dolt: Some(Dolt::Differs("Postgres raises division_by_zero, where MySQL returns NULL")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT div(1, 0) ;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's DIV operator is div()")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: delete all rows in table",
            intent: "DELETE without WHERE empties the table.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk;",
                    expected: Expected::Tag("DELETE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM one_pk;",
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
            name: "sql: insert on duplicate key inserts data by column",
            intent: "An upsert assigns the named columns from the conflicting row or from constants.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (col_a varchar(2) NOT NULL, col_b varchar(2), col_c varchar(2), PRIMARY KEY(col_a));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'created table');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (col_a,col_b) VALUES('a', 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (col_a,col_b,col_c) VALUES ('a','','b')  ON CONFLICT (\"col_a\") DO UPDATE SET col_a = test.col_a, col_b = test.col_b, col_c = EXCLUDED.col_c;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ON DUPLICATE KEY UPDATE reads a bare column as the existing row's, which Postgres names with the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE col_a = 'a';",
                    expected: Expected::Rows {
                        columns: &[Column("col_a", VARCHAR), Column("col_b", VARCHAR), Column("col_c", VARCHAR)],
                        rows: &[
                            &[T("a"), T("b"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES ('b','b','b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES ('b', '1', '1')  ON CONFLICT (\"col_a\") DO UPDATE SET col_b = '2', col_c='2';",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE col_a = 'b';",
                    expected: Expected::Rows {
                        columns: &[Column("col_a", VARCHAR), Column("col_b", VARCHAR), Column("col_c", VARCHAR)],
                        rows: &[
                            &[T("b"), T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES ('c', 'c', 'c'), ('c', '1', '1')  ON CONFLICT (\"col_a\") DO UPDATE SET col_b = '2', col_c='2';",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "ON CONFLICT DO UPDATE command cannot affect row a second time", hint: "Ensure that no rows proposed for insertion within the same command have duplicate constrained values.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres' ON CONFLICT refuses a statement that reaches one row twice, where MySQL applies the rows in turn")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE col_a = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("col_a", VARCHAR), Column("col_b", VARCHAR), Column("col_c", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES ('d','d','d');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test WHERE col_a='d';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES ('d', '1', '1')  ON CONFLICT (\"col_a\") DO UPDATE SET col_b = '2', col_c='2';",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE col_a = 'd';",
                    expected: Expected::Rows {
                        columns: &[Column("col_a", VARCHAR), Column("col_b", VARCHAR), Column("col_c", VARCHAR)],
                        rows: &[
                            &[T("d"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: duplicate key inserts on table with primary and secondary indexes",
            intent: "An insert that duplicates a unique secondary key is refused and leaves the table unchanged.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int PRIMARY KEY, uk int UNIQUE, i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's UNIQUE KEY column clause is UNIQUE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES(0,0,0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("uk", INT4), Column("i", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,uk) VALUES(1,0) ;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_uk_key\"", detail: "Key (uk)=(0) already exists.", schema: "public", table: "test", constraint: "test_uk_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("uk", INT4), Column("i", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: active_branch() func",
            intent: "active_branch() names the checked out branch.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: active_branch() func on feature branch",
            intent: "active_branch() names a branch after a dolt_checkout to it.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('tmp_br');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('tmp_br');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("tmp_br")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE name = (SELECT active_branch());",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("tmp_br")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: autocommit = off",
            intent: "Rows inserted in an open transaction are visible inside it and kept by COMMIT.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'clean working set');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (3), (5);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t1;",
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
                    query: "INSERT INTO t1 VALUES (3), (5);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "sql: dolt diff table correctly works with IN",
            intent: "dolt_diff_<table> filters by IN on commit hashes.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Commit 1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_mytable WHERE dolt_diff_mytable.dolt_to_commit IN (current_setting('test.head'), '00200202');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'Commit 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head2', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_mytable WHERE dolt_diff_mytable.dolt_to_commit IN (current_setting('test.head'), current_setting('test.head2'), 'fake-val');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: dolt diff table correctly works with NOT and/or IS NULL",
            intent: "dolt_diff_<table> filters by IS NULL and IS NOT NULL on its from columns.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'new table t');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add 1, 2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t WHERE from_pk IS NULL;",
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
                    query: "UPDATE t SET pk = 3 WHERE pk = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add 3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t WHERE from_pk IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "sql: dolt diff table correctly works with datetime comparisons",
            intent: "dolt_diff_<table> filters by comparisons on its commit dates.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'new table t');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1), (2), (3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add 1, 2, 3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET pk = 4 WHERE pk = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t WHERE dolt_to_commit_date IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t WHERE dolt_to_commit_date < now();",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's UTC_TIMESTAMP(6) is now()")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: sql print on order by returns the correct result",
            intent: "ORDER BY returns every row in key order.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable VALUES (1),(2),(3),(4),(5),(6),(7),(8),(9),(10),(11),(12),(13),(14),(15),(16),(17),(18),(19),(20);",
                    expected: Expected::Tag("INSERT 0 20"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                            &[T("10")],
                            &[T("11")],
                            &[T("12")],
                            &[T("13")],
                            &[T("14")],
                            &[T("15")],
                            &[T("16")],
                            &[T("17")],
                            &[T("18")],
                            &[T("19")],
                            &[T("20")],
                        ],
                        tag: "SELECT 20",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: simple update against join works",
            intent: "An UPDATE joined to another table updates the matching rows.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test2 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET val = 2 FROM test2 WHERE test.pk = test2.pk;",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's UPDATE ... JOIN is UPDATE ... FROM")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
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
        ScriptTest {
            name: "sql: covering indexes",
            intent: "Lookups through a secondary index that covers the query return the full rows.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test4 (a int NOT NULL, b int NOT NULL, c int NOT NULL, d int NOT NULL, PRIMARY KEY (a,b,c,d));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t4bc ON test4 (b,c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (a int NOT NULL, b int, c int, PRIMARY KEY (a));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t2bc ON test2 (b,c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test4 VALUES (1,2,3,4), (5,6,7,8);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test2 VALUES (1,2,3), (4,5,6);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test4 WHERE b = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("5"), T("6"), T("7"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test4 WHERE a = 5 AND b = 6 AND c = 7 AND d = 8;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("5"), T("6"), T("7"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test4 WHERE a = 5 AND b = 6 AND c = 7 AND d = 8;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("5"), T("6"), T("7"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2 WHERE b = 5 AND c = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("4"), T("5"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: can insert datetime with golang time struct zero value 0001-01-01 00:00:00",
            intent: "A timestamp column takes the earliest timestamp 0001-01-01 00:00:00.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE dts (created_at timestamp NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dts (\"created_at\") VALUES ('0001-01-01 00:00:00');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql: AS OF queries",
            intent: "A table read AS OF a branch, a commit hash, an ancestor expression, or a time reads its rows at that revision, a dropped table included, and a time before every commit finds no table.",
            set_up_script: &[
                "CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                "CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';",
                "INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);",
                "INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);",
                "INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Initial main commit', '--date', '2020-03-01T12:00:00Z');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.main_commit', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'new_branch');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Differs("Dolt's SQL dolt_checkout leaves the working changes on the branch it leaves, where the CLI's dolt checkout carries them, so the port changes the tables after the checkout")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE one_pk SET c1 = c1 + 1;",
                    expected: Expected::Tag("UPDATE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE two_pk;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Updated a table, dropped a table', '--date', '2020-03-01T13:00:00Z');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('test.new_commit', dolt_hashof('HEAD'), false);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("1"), T("11")],
                            &[T("2"), T("21")],
                            &[T("3"), T("31")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF 'main' ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF current_setting('test.main_commit') ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM two_pk AS OF 'main';",
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
                    query: "SELECT COUNT(*) FROM two_pk AS OF current_setting('test.main_commit');",
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
                    query: "SELECT pk,c1 FROM one_pk AS OF 'HEAD~' ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF 'new_branch^' ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF 'new_branch' ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("1"), T("11")],
                            &[T("2"), T("21")],
                            &[T("3"), T("31")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF current_setting('test.new_commit') ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("1"), T("11")],
                            &[T("2"), T("21")],
                            &[T("3"), T("31")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('new_branch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET TIME ZONE 'UTC';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF TIMESTAMP '2020-03-01 12:00:00' ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF TIMESTAMP '2020-03-01 12:15:00' ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF TIMESTAMP '2020-03-01 13:00:00' ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("1"), T("11")],
                            &[T("2"), T("21")],
                            &[T("3"), T("31")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF TIMESTAMP '2020-03-01 13:15:00' ORDER BY c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("1"), T("11")],
                            &[T("2"), T("21")],
                            &[T("3"), T("31")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk,c1 FROM one_pk AS OF TIMESTAMP '2020-03-01 11:59:59' ORDER BY c1;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"one_pk\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: branch qualified DB name in select"#,
            intent: r#"A table a branch's database made reads through the branch-qualified name."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a1(x int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM "postgres/feature-branch".public.a1 ORDER BY x;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
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
        ScriptTest {
            name: r#"sql: branch qualified DB name in insert"#,
            intent: r#"An insert through a branch-qualified name writes the branch."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a1(x int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO "postgres/feature-branch".public.a1 VALUES (4);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM "postgres/feature-branch".public.a1 ORDER BY x;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: commit hash qualified DB name in select"#,
            intent: r#"A commit-qualified name reads the table as that commit holds it."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
                r#"CREATE FUNCTION a1_at(rev text) RETURNS SETOF int LANGUAGE plpgsql AS $$BEGIN RETURN QUERY EXECUTE format('SELECT x FROM %I.public.a1 ORDER BY x', 'postgres/' || rev); END$$;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a1(x int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (4), (5), (6);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'more values');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM a1_at(dolt_hashof('feature-branch~1'));"#,
                    expected: Expected::Rows {
                        columns: &[Column("a1_at", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("the port names the commit by its hash at run time through EXECUTE, since a test's statements are fixed text")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: commit hash qualified DB name in delete"#,
            intent: r#"A delete through a commit-qualified name is refused as a write to a read-only revision."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a1(x int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (4), (5), (6);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'more values');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$BEGIN EXECUTE format('DELETE FROM %I.public.a1', 'postgres/' || dolt_hashof('feature-branch~1')); END$$;"#,
                    expected: Expected::Error(Diagnostic { code: "25006", message: r#"cannot execute DELETE in a read-only transaction"#, ..E }),
                    dolt: Some(Dolt::Differs("Postgres' error for a write in a read-only transaction, where Dolt's names the revision database read-only")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: commit hash qualified DB name in update"#,
            intent: r#"A update through a commit-qualified name is refused as a write to a read-only revision."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a1(x int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (4), (5), (6);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'more values');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$BEGIN EXECUTE format('UPDATE %I.public.a1 SET x = x*10', 'postgres/' || dolt_hashof('feature-branch~1')); END$$;"#,
                    expected: Expected::Error(Diagnostic { code: "25006", message: r#"cannot execute UPDATE in a read-only transaction"#, ..E }),
                    dolt: Some(Dolt::Differs("Postgres' error for a write in a read-only transaction, where Dolt's names the revision database read-only")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: tag qualified DB name in select"#,
            intent: r#"A tag-qualified name reads the table as the tagged commit holds it."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a1(x int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("dolt tag is a CLI command, which dolt_tag() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM "postgres/v1".public.a1 ORDER BY x;"#,
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
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
        ScriptTest {
            name: r#"sql: USE tag doesn't create duplicate commit DB name"#,
            intent: r#"Using a branch, tagging it, using the tag, and checking out the branch from the tag's database all work."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_checkout('feature-branch');"#,
                    expected: Expected::Ok,
                    client: "v1",
                    database: "postgres/v1",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: USE fake hash throws error"#,
            intent: r#"Connecting to a revision database whose hash names no commit fails."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT 1;"#,
                    expected: Expected::ClientError("FATAL: database \"postgres/h4jks5lomp9u41r6902knn0pfr7lsgth\" does not exist (SQLSTATE 3D000)"),
                    client: "h4jks5lo",
                    database: "postgres/h4jks5lomp9u41r6902knn0pfr7lsgth",
                    dolt: Some(Dolt::Differs("USE is MySQL's, so the port connects to the revision's database, which Postgres refuses as a database that does not exist, where Dolt's error is database not found")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: tag qualified DB name in delete"#,
            intent: r#"A delete through a tag-qualified name, or in the tag's database, is refused as a write to a read-only revision."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a1(x int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (4), (5), (6);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'more values');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("dolt tag is a CLI command, which dolt_tag() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM "postgres/v1".public.a1;"#,
                    expected: Expected::Error(Diagnostic { code: "25006", message: r#"cannot execute DELETE in a read-only transaction"#, ..E }),
                    dolt: Some(Dolt::Differs("Postgres' error for a write in a read-only transaction, where Dolt's names the revision database read-only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM a1;"#,
                    expected: Expected::Error(Diagnostic { code: "25006", message: r#"cannot execute DELETE in a read-only transaction"#, ..E }),
                    client: "v1",
                    database: "postgres/v1",
                    dolt: Some(Dolt::Differs("Postgres' error for a write in a read-only transaction, where Dolt's names the revision database read-only")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: tag qualified DB name in update"#,
            intent: r#"A update through a tag-qualified name, or in the tag's database, is refused as a write to a read-only revision."#,
            set_up_script: &[
                r#"CREATE TABLE one_pk (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));"#,
                r#"CREATE TABLE two_pk (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));"#,
                r#"CREATE TABLE has_datetimes (pk bigint NOT NULL, date_created timestamp, PRIMARY KEY (pk)); COMMENT ON COLUMN has_datetimes.pk IS 'tag:0'; COMMENT ON COLUMN has_datetimes.date_created IS 'tag:1';"#,
                r#"INSERT INTO one_pk (pk,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0),(1,10,10,10,10,10),(2,20,20,20,20,20),(3,30,30,30,30,30);"#,
                r#"INSERT INTO two_pk (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,0,0,0,0,0),(0,1,10,10,10,10,10),(1,0,20,20,20,20,20),(1,1,30,30,30,30,30);"#,
                r#"INSERT INTO has_datetimes (pk, date_created) VALUES (0, '2020-02-17 00:00:00');"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'commit tables');"#,
                r#"SELECT dolt_branch('feature-branch');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE a1(x int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("USE is MySQL's, so the port connects to the revision's database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'new table');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO a1 VALUES (4), (5), (6);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'more values');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_tag('v1');"#,
                    expected: Expected::Ok,
                    client: "feature-",
                    database: "postgres/feature-branch",
                    dolt: Some(Dolt::MySqlOnly("dolt tag is a CLI command, which dolt_tag() is")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE "postgres/v1".public.a1 SET x = x*10;"#,
                    expected: Expected::Error(Diagnostic { code: "25006", message: r#"cannot execute UPDATE in a read-only transaction"#, ..E }),
                    dolt: Some(Dolt::Differs("Postgres' error for a write in a read-only transaction, where Dolt's names the revision database read-only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE a1 SET x = x*10;"#,
                    expected: Expected::Error(Diagnostic { code: "25006", message: r#"cannot execute UPDATE in a read-only transaction"#, ..E }),
                    client: "v1",
                    database: "postgres/v1",
                    dolt: Some(Dolt::Differs("Postgres' error for a write in a read-only transaction, where Dolt's names the revision database read-only")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: create new database"#,
            intent: r#"CREATE DATABASE makes a database whose tables and commits are its own."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test1;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE table_a (x INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "test1",
                    database: "test1",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO table_a VALUES (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-a', '-m', 'created table_a');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"table_a"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM dolt_log WHERE message = 'created table_a';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test1",
                    database: "test1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test2;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT current_database();"#,
                    expected: Expected::Rows {
                        columns: &[Column("current_database", NAME)],
                        rows: &[
                            &[T(r#"postgres"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT datname FROM pg_database WHERE datname IN ('postgres', 'test1', 'test2') ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T(r#"postgres"#)],
                            &[T(r#"test1"#)],
                            &[T(r#"test2"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"SHOW DATABASES is MySQL's, so the port reads pg_database, and the CLI's checks for files and directories of the new database's name are its own"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: use database with multiple dbs"#,
            intent: r#"Each database keeps the tables made in it."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE db1;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE db2;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t1 (a INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "db1",
                    database: "db1",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t1 VALUES (10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t2 (a INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "db2",
                    database: "db2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 VALUES (20);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "db2",
                    database: "db2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T(r#"10"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db1",
                    database: "db1",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T(r#"20"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db2",
                    database: "db2",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: drop database with branches in use"#,
            intent: r#"Dropping a database while a session uses one of its branches succeeds, and the branch can't be connected to afterward."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test1;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE test2;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_checkout('-b', 'newBranch');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"Switched to branch 'newBranch'"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test1",
                    database: "test1",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT active_branch();"#,
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T(r#"newBranch"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "branch",
                    database: "test1/newBranch",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DATABASE Test1 WITH (FORCE);"#,
                    expected: Expected::Tag("DROP DATABASE"),
                    client: "test2",
                    database: "test2",
                    dolt: Some(Dolt::MySqlOnly(r#"Postgres refuses to drop a database other sessions are connected to without FORCE"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "test2",
                    database: "test2",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 1;"#,
                    expected: Expected::ClientError("does not exist"),
                    client: "after",
                    database: "test1/newBranch",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"sql: dolt diff table respects qualified database"#,
            intent: r#"dolt_diff lists the changes of its own database only."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE db01;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE db02;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t01 (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "db01",
                    database: "db01",
                    dolt: Some(Dolt::MySqlOnly(r#"MySQL's USE switches databases, and a Postgres session connects to one, so the port runs each statement on a connection to its database"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM dolt_diff;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T(r#"public.t01"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "db01",
                    database: "db01",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM dolt_diff;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "db02",
                    database: "db02",
                    dolt: Some(Dolt::MySqlOnly(r#"db01.dolt_diff names a table of another database, which Postgres has no form of, so the port reads each database's own dolt_diff"#)),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
