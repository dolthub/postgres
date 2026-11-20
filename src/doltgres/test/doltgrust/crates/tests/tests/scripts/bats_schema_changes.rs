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

//! Dolt's integration-tests/bats/schema-changes.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_schema_changes() {
    run_scripts(&[
        ScriptTest {
            name: "schema-changes: changing column types should not produce a data diff error",
            intent: "Dropping a column and adding it back with another type diffs without error.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'made table');",
                "ALTER TABLE test DROP COLUMN c1;",
                "ALTER TABLE test ADD COLUMN c1 text;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint,\n    c1 text\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows of Postgres' CREATE TABLE text")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, to_pk, to_c1, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT8), Column("from_c1", INT8), Column("to_pk", INT8), Column("to_c1", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("0"), Null, T("modified")],
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
            name: "schema-changes: dolt schema alter column preserves table checks",
            intent: "Renaming a column keeps the table's check constraints.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "ALTER TABLE test ADD CONSTRAINT test_check CHECK (c2 < 12345);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN c1 TO c0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'test'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("test_check"), T("CHECK ((c2 < 12345))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints MySQL's CREATE TABLE, so pg_constraint shows the check")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: dolt schema rename column",
            intent: "A renamed column keeps its place, type, and values.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN c1 TO c0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                            &[T("c0"), T("bigint")],
                            &[T("c2"), T("bigint")],
                            &[T("c3"), T("bigint")],
                            &[T("c4"), T("bigint")],
                            &[T("c5"), T("bigint")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints MySQL's CREATE TABLE, so information_schema.columns shows the columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c0", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: dolt schema rename column fails when column is used in table check",
            intent: "Renaming a column that a check constraint uses carries the rename into the constraint.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "ALTER TABLE test ADD CONSTRAINT test_check CHECK (c2 < 12345);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test RENAME COLUMN c2 TO c0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres renames the column inside the check constraint, where Dolt refuses the rename")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'test'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("test_check"), T("CHECK ((c0 < 12345))")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: dolt schema delete column",
            intent: "A dropped column leaves the table's other columns and values.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN c1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint")],
                            &[T("c2"), T("bigint")],
                            &[T("c3"), T("bigint")],
                            &[T("c4"), T("bigint")],
                            &[T("c5"), T("bigint")],
                        ],
                        tag: "SELECT 5",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints MySQL's CREATE TABLE, so information_schema.columns shows the columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: dolt diff on schema changes",
            intent: "An added column is a schema diff with no data diff until a row is inserted.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'committed table so we can see diffs');",
                "ALTER TABLE test ADD c0 bigint;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint,\n    c0 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows of Postgres' CREATE TABLE text")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', 'test');",
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
                    query: "INSERT INTO test (pk, c0, c1, c2, c3, c4, c5) VALUES (0, 0, 0, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c0, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT8), Column("to_c0", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN c0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', 'test');",
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
            name: "schema-changes: dolt diff on schema changes rename primary key",
            intent: "Renaming the primary key column shows in the schema diff.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'committed table so we can see diffs');",
                "ALTER TABLE test RENAME COLUMN pk TO pk1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk1 bigint CONSTRAINT test_pk_not_null NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk1);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows of Postgres' CREATE TABLE text")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: adding and dropping column should produce no diff",
            intent: "Adding a column and dropping it again leaves no diff.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'committed table so we can see diffs');",
                "ALTER TABLE test ADD c0 bigint;",
                "ALTER TABLE test DROP COLUMN c0;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints nothing, where dolt_status and dolt_diff_summary() return no rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: schema diff should show primary keys in output",
            intent: "The schema diff of a renamed column shows the primary key.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'committed table so we can see diffs');",
                "ALTER TABLE test RENAME COLUMN c2 TO column2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    column2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows of Postgres' CREATE TABLE text")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: changing column does not allow nulls in primary key",
            intent: "A renamed primary key column still refuses NULL.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk1 bigint, pk2 bigint, v1 bigint, PRIMARY KEY (pk1, pk2));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test2 (pk1, pk2) VALUES (1, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"pk2\" of relation \"test2\" violates not-null constraint", detail: "Failing row contains (1, null, null).", schema: "public", table: "test2", column: "pk2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 RENAME COLUMN pk2 TO pk2new;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CHANGE becomes RENAME COLUMN")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test2 (pk1, pk2new) VALUES (1, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"pk2new\" of relation \"test2\" violates not-null constraint", detail: "Failing row contains (1, null, null).", schema: "public", table: "test2", column: "pk2new", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: changing column types in place works",
            intent: "Changing column types in place diffs the schema, commits, and leaves a branch from before the change usable.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk1 bigint, pk2 bigint, v1 varchar(100) NOT NULL, v2 varchar(120) NULL, PRIMARY KEY (pk1, pk2));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test2 (pk1, pk2, v1, v2) VALUES (1, 1, 'abc', 'def');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'Created table with one row')) > 0;",
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
                    query: "SELECT dolt_branch('original');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_remote('add', 'origin', 'file://remotedir');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_push('origin', 'main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_clone('file://remotedir', 'original');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE Test2 ALTER COLUMN V1 TYPE varchar(300);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MODIFY becomes ALTER COLUMN TYPE and SET NOT NULL, tinyint becomes smallint, and dolt schema tags are left out, since Doltgrust has no column tags")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE TEST2 ALTER COLUMN PK2 TYPE smallint;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE Test2 ALTER COLUMN V2 TYPE varchar(1024), ALTER COLUMN V2 SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test2 (\n    pk1 bigint NOT NULL,\n    pk2 bigint NOT NULL,\n    v1 character varying(100) NOT NULL,\n    v2 character varying(120)\n);\n\nALTER TABLE public.test2 OWNER TO postgres;\n\nALTER TABLE ONLY public.test2\n    ADD CONSTRAINT test2_pkey PRIMARY KEY (pk1, pk2);"), T("CREATE TABLE public.test2 (\n    pk1 bigint NOT NULL,\n    pk2 smallint NOT NULL,\n    v1 character varying(300) NOT NULL,\n    v2 character varying(1024) NOT NULL\n);\n\nALTER TABLE public.test2 OWNER TO postgres;\n\nALTER TABLE ONLY public.test2\n    ADD CONSTRAINT test2_pkey PRIMARY KEY (pk1, pk2);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows of Postgres' CREATE TABLE text")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'Changed types')) > 0;",
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
                    query: "SELECT dolt_checkout('original');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"Switched to branch 'original'\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test2 (pk1, pk2, v1, v2) VALUES (2, 2, 'abc', 'def');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'Created table with one row')) > 0;",
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
                    query: "SELECT * FROM test2 ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", VARCHAR), Column("v2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("abc"), T("def")],
                            &[T("2"), T("2"), T("abc"), T("def")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: drop then add column",
            intent: "A column dropped and added again comes back NULL rather than with its old values.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk1 bigint, pk2 bigint, v1 bigint, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO test2 VALUES (1, 1, 1), (2, 2, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Committing test table');",
                "ALTER TABLE test2 DROP COLUMN v1;",
                "ALTER TABLE test2 ADD COLUMN v1 bigint;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2 WHERE pk1 = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: drop then add column show no schema diff",
            intent: "A column dropped and added again diffs every row's old value against NULL.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk1 bigint, pk2 bigint, v1 bigint, PRIMARY KEY (pk1, pk2));",
                "INSERT INTO test2 VALUES (1, 1, 1), (2, 2, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'Committing test table');",
                "ALTER TABLE test2 DROP COLUMN v1;",
                "ALTER TABLE test2 ADD COLUMN v1 bigint;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2 WHERE pk1 = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk1, from_pk2, from_v1, to_pk1, to_pk2, to_v1, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test2') ORDER BY from_pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk1", INT8), Column("from_pk2", INT8), Column("from_v1", INT8), Column("to_pk1", INT8), Column("to_pk2", INT8), Column("to_v1", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("1"), T("1"), Null, T("modified")],
                            &[T("2"), T("2"), T("2"), T("2"), T("2"), Null, T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema-changes: regression test for merging check constraints with TEXT type panicking due to a nil sql ctx",
            intent: "Merging a branch that adds a check constraint on a text column keeps the constraint.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 text);",
                "SELECT dolt_commit('-Am', 'initial');",
                "SELECT dolt_branch('right');",
                "INSERT INTO t VALUES (1, 'valid');",
                "SELECT dolt_commit('-am', 'row');",
                "SELECT dolt_checkout('right');",
                "ALTER TABLE t ADD CONSTRAINT col1_check CHECK (col1 = 'valid');",
                "SELECT dolt_commit('-am', 'add check');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("col1_check"), T("CHECK ((col1 = 'valid'::text))")],
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
