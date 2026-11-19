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

//! Dolt's go/libraries/doltcore/sqle/enginetest/system_table_function_index_tests.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_system_table_function_index_tests() {
    run_scripts(&[
        ScriptTest {
            name: "DOLT_DIFF() with secondary index basic use case",
            intent: "Filters and lookup joins on dolt_diff's to_ and from_ columns return the same rows an unfiltered diff holds, through the table's secondary indexes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20)); CREATE INDEX covering ON t (c1, c2); CREATE INDEX non_covering ON t (c2);",
                "INSERT INTO t VALUES(1, 'foo', 'a'), (2, 'bar', 'b'), (3, 'foo', 'c'), (4, 'bar', 'd');",
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (5, 'foo', 'e'), (6, 'bar', 'f');",
                "DELETE FROM t WHERE pk <= 2;",
                "UPDATE t SET c1 = 'bar' WHERE pk = 3;",
                "UPDATE t SET c1 = 'foo' WHERE pk = 4;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'insert rows');",
                "SELECT set_config('test.Commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE lookup_vals (val varchar(20));",
                "INSERT INTO lookup_vals VALUES ('foo'), ('bar');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("foo"), T("a"), T("removed")],
                            &[Null, Null, Null, T("2"), T("bar"), T("b"), T("removed")],
                            &[T("3"), T("bar"), T("c"), T("3"), T("foo"), T("c"), T("modified")],
                            &[T("4"), T("foo"), T("d"), T("4"), T("bar"), T("d"), T("modified")],
                            &[T("5"), T("foo"), T("e"), Null, Null, Null, T("added")],
                            &[T("6"), T("bar"), T("f"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c1 = 'foo' ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("foo"), T("d"), T("4"), T("bar"), T("d"), T("modified")],
                            &[T("5"), T("foo"), T("e"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c1 = 'bar';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), T("bar"), T("c"), T("3"), T("foo"), T("c"), T("modified")],
                            &[T("6"), T("bar"), T("f"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE from_c1 = 'foo' ORDER BY from_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("foo"), T("a"), T("removed")],
                            &[T("3"), T("bar"), T("c"), T("3"), T("foo"), T("c"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE from_c1 = 'bar';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, Null, T("2"), T("bar"), T("b"), T("removed")],
                            &[T("4"), T("foo"), T("d"), T("4"), T("bar"), T("d"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c2 = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("3"), T("bar"), T("c"), T("3"), T("foo"), T("c"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ JOIN_ORDER(lookup_vals,dolt_diff) LOOKUP_JOIN(lookup_vals,dolt_diff) */ val, to_pk, to_c1, dolt_diff_type FROM lookup_vals JOIN dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') ON lookup_vals.val = to_c1;",
                    expected: Expected::Rows {
                        columns: &[Column("val", VARCHAR), Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("bar"), T("3"), T("bar"), T("modified")],
                            &[T("bar"), T("6"), T("bar"), T("added")],
                            &[T("foo"), T("4"), T("foo"), T("modified")],
                            &[T("foo"), T("5"), T("foo"), T("added")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit0', TRUE), current_setting('test.Commit1', TRUE), 't') WHERE to_c1 = 'foo' ORDER BY to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("foo"), T("a"), Null, Null, Null, T("added")],
                            &[T("3"), T("foo"), T("c"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test DOLT_DIFF() secondary index with generated columns",
            intent: "Filters on dolt_diff's columns of a table whose indexes cover a generated column return the right rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int, c3 int GENERATED ALWAYS AS (pk+c1+c2) STORED); CREATE INDEX covering ON t (c2, c1); CREATE INDEX covering_with_virtual ON t (c1, c2, c3);",
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t (pk, c1, c2) VALUES(1, 2, 3), (4, 5, 6);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'insert rows');",
                "SELECT set_config('test.Commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, to_c3, from_pk, from_c1, from_c2, from_c3, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c2 = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("to_c3", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("from_c3", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("6"), Null, Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres can't index a virtual generated column, so c3 is stored and the diff shows its value, where Dolt's diff shows NULL for a virtual column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, to_c3, from_pk, from_c1, from_c2, from_c3, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c1 = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("to_c3", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("from_c3", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("5"), T("6"), T("15"), Null, Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres can't index a virtual generated column, so c3 is stored and the diff shows its value, where Dolt's diff shows NULL for a virtual column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test DOLT_DIFF() with expression index",
            intent: "A filter on an expression of dolt_diff's columns that a table's expression index covers returns the right rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "CREATE INDEX expression_index ON t ((c1+c2));",
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t (pk, c1, c2) VALUES(1, 2, 3), (4, 5, 6);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'insert rows');",
                "SELECT set_config('test.Commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c1 + to_c2 = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Bug("Dolt's expectation of the added row (1, 2, 3) is right, but Dolt main fails this filter through its expression index, so Dolt skips the script")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test DOLT_DIFF() with unusual primary index ordinals",
            intent: "A filter on dolt_diff's columns works for a table whose primary key lists its columns out of order.",
            set_up_script: &[
                "CREATE TABLE t (c0 int, c1 int, c2 int, c3 int, c4 int, PRIMARY KEY (c4, c2, c0)); CREATE INDEX sec_idx ON t (c3, c1);",
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t (c0, c1, c2, c3, c4) VALUES(0, 1, 2, 3, 4), (5, 6, 7, 8, 9);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'insert rows');",
                "SELECT set_config('test.Commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_c0, to_c1, to_c2, to_c3, to_c4, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c3 = 8;",
                    expected: Expected::Rows {
                        columns: &[Column("to_c0", INT4), Column("to_c1", INT4), Column("to_c2", INT4), Column("to_c3", INT4), Column("to_c4", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("5"), T("6"), T("7"), T("8"), T("9"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_DIFF() cannot use secondary indexes if they have different schemas",
            intent: "Filters on dolt_diff's columns return the right rows when the table's secondary index changed between the two commits.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20)); CREATE INDEX sec_idx ON t (c1, c2);",
                "INSERT INTO t VALUES(1, 'foo', 'a'), (2, 'bar', 'b'), (3, 'foo', 'c'), (4, 'bar', 'd');",
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'create table');",
                "DROP INDEX sec_idx;",
                "CREATE INDEX sec_idx ON t(c2, c1);",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (5, 'foo', 'e'), (6, 'bar', 'f');",
                "DELETE FROM t WHERE pk <= 2;",
                "UPDATE t SET c1 = 'bar' WHERE pk = 3;",
                "UPDATE t SET c1 = 'foo' WHERE pk = 4;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'insert rows');",
                "SELECT set_config('test.Commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE lookup_vals (val varchar(20));",
                "INSERT INTO lookup_vals VALUES ('foo'), ('bar');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c1 = 'foo';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("4"), T("foo"), T("d"), T("modified")],
                            &[T("5"), T("foo"), T("e"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c1, to_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE to_c2 = 'e';",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("to_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("5"), T("foo"), T("e"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE from_c1 = 'foo';",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("foo"), T("a"), T("removed")],
                            &[T("3"), T("foo"), T("c"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't') WHERE from_c2 = 'a';",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("from_c2", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("foo"), T("a"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "test DOLT_PATCH() indexes",
            intent: "Filters, lookup joins, and correlated subqueries on dolt_patch's diff_type return each kind's statements.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(20), c2 varchar(20));",
                "INSERT INTO t VALUES(1, 'one', 'two');",
                "SELECT set_config('test.Commit0', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit one');",
                "SELECT set_config('test.Commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE diff_type_name(name varchar(20), t varchar(20));",
                "INSERT INTO diff_type_name VALUES ('s', 'schema'), ('d', 'data');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit0', TRUE), current_setting('test.Commit1', TRUE), 't') WHERE diff_type = 'schema';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("public.t"), T("schema"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    c1 character varying(20),\n    c2 character varying(20)\n);")],
                            &[T("public.t"), T("schema"), T("ALTER TABLE public.t OWNER TO postgres;")],
                            &[T("public.t"), T("schema"), T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, diff_type, statement FROM dolt_patch(current_setting('test.Commit0', TRUE), current_setting('test.Commit1', TRUE), 't') WHERE diff_type = 'data';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("public.t"), T("data"), T("INSERT INTO public.t (pk, c1, c2) VALUES (1, 'one', 'two');")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ JOIN_ORDER(diff_type_name,dolt_patch) LOOKUP_JOIN(diff_type_name,dolt_patch) */ name, t, table_name, statement FROM diff_type_name JOIN dolt_patch(current_setting('test.Commit0', TRUE), current_setting('test.Commit1', TRUE), 't') ON diff_type_name.t = diff_type;",
                    expected: Expected::Rows {
                        columns: &[Column("name", VARCHAR), Column("t", VARCHAR), Column("table_name", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("s"), T("schema"), T("public.t"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    c1 character varying(20),\n    c2 character varying(20)\n);")],
                            &[T("s"), T("schema"), T("public.t"), T("ALTER TABLE public.t OWNER TO postgres;")],
                            &[T("s"), T("schema"), T("public.t"), T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);")],
                            &[T("d"), T("data"), T("public.t"), T("INSERT INTO public.t (pk, c1, c2) VALUES (1, 'one', 'two');")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ( SELECT statement FROM (SELECT * FROM dolt_patch(current_setting('test.Commit0', TRUE), current_setting('test.Commit1', TRUE), 't') WHERE diff_type = diff_type_name.t) AS rhs) FROM diff_type_name;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "more than one row returned by a subquery used as an expression", ..E }),
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, with schema-qualified table names, Appendix C \"dolt_patch statements\"; its three schema statements make the correlated subquery return more than one row")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
