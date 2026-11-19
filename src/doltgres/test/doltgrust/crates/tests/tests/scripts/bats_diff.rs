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

//! Dolt's integration-tests/bats/diff.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};

#[test]
fn test_diff() {
    run_scripts(&[
        ScriptTest {
            name: "diff: clean working set",
            intent: "With everything committed, the working set diffs to nothing, and HEAD^ to HEAD shows the row.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'table');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD^', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD^', 'HEAD', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'HEAD^', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("removed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD..HEAD', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD^..HEAD', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD..HEAD^', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("removed")],
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
            name: "diff: dirty working set",
            intent: "An unstaged row shows against STAGED and HEAD, and once staged only against HEAD.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'table');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
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
            name: "diff: two and three dot diff",
            intent: "Two revisions or a two-dot range compare the tips, and a three-dot range compares from their merge base.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'table');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'row');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (2, 2, 2, 2, 2, 2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'newrow');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('branch1', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("removed")],
                            &[T("2"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('branch1..main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("removed")],
                            &[T("2"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('branch1', 'main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("removed")],
                            &[T("2"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('main..branch1', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("added")],
                            &[T("2"), T("removed")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('main', 'branch1', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("added")],
                            &[T("2"), T("removed")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('branch1...main', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('main...branch1', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("added")],
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
            name: "diff: data and schema changes",
            intent: "A diff shows both the schema change of a table and its changed rows.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int, c2 int);",
                "INSERT INTO test VALUES (1, 2, 3);",
                "INSERT INTO test VALUES (4, 5, 6);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'First commit');",
                "ALTER TABLE test DROP COLUMN c2, ADD COLUMN c3 varchar(10);",
                "INSERT INTO test VALUES (7, 8, 9);",
                "DELETE FROM test WHERE pk = 1;",
                "UPDATE test SET c1 = 100 WHERE pk = 4;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("CREATE TABLE public.test (\n    pk integer NOT NULL,\n    c1 integer,\n    c2 integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk integer NOT NULL,\n    c1 integer,\n    c3 character varying(10)\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, from_c2, to_pk, to_c1, to_c3, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test') ORDER BY 1, 4;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", INT4), Column("from_c2", INT4), Column("to_pk", INT4), Column("to_c1", INT4), Column("to_c3", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), Null, Null, Null, T("removed")],
                            &[T("4"), T("5"), T("6"), T("4"), T("100"), Null, T("modified")],
                            &[Null, Null, Null, T("7"), T("8"), T("9"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: data diff only",
            intent: "A diff with only data changes shows no schema change.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'First commit');",
                "INSERT INTO test (pk) VALUES (10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("10"), T("added")],
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
            name: "diff: schema changes only",
            intent: "A diff with only schema changes shows them and no changed rows.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'First commit');",
                "ALTER TABLE test DROP COLUMN c3, ADD COLUMN c6 varchar(10);",
                "ALTER TABLE test ALTER COLUMN c4 TYPE smallint;",
                "COMMENT ON COLUMN test.c4 IS 'new comment';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c4 smallint,\n    c5 bigint,\n    c6 character varying(10)\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nCOMMENT ON COLUMN public.test.c4 IS 'new comment';\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's ADD COLUMN ... AFTER and tinyint have no Postgres form; c6 goes last and c4 becomes smallint")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: with table args",
            intent: "A diff of one table shows only its rows, and a table neither revision has is refused.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE other (pk int NOT NULL PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'tables');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "INSERT INTO other VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("9"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'STAGED', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'STAGED', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("9"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD^', 'HEAD', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD^..HEAD', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("9"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD^', 'HEAD', 'fake');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"fake\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD^..HEAD', 'fake');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"fake\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: with index and foreign key changes",
            intent: "A schema diff shows dropped and added indexes and foreign keys.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE parent (pk bigint PRIMARY KEY, c1 bigint, c2 bigint);",
                "CREATE UNIQUE INDEX c1 ON parent (c1);",
                "ALTER TABLE test ADD CONSTRAINT fk1 FOREIGN KEY (c1) REFERENCES parent(c1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'added parent table, foreign key');",
                "CREATE UNIQUE INDEX c2 ON parent (c2);",
                "ALTER TABLE test DROP CONSTRAINT fk1;",
                "DROP INDEX c1;",
                "ALTER TABLE test ADD CONSTRAINT fk2 FOREIGN KEY (c2) REFERENCES parent(c2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT fk1 FOREIGN KEY (c1) REFERENCES public.parent(c1);"), T("CREATE TABLE public.test (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint,\n    c3 bigint,\n    c4 bigint,\n    c5 bigint\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT fk2 FOREIGN KEY (c2) REFERENCES public.parent(c2);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("a foreign key needs a unique index on the columns it references in Postgres, and MySQL's index made for a foreign key's own columns has no Postgres counterpart")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 'parent');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.parent"), T("public.parent"), T("CREATE TABLE public.parent (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint\n);\n\nALTER TABLE public.parent OWNER TO postgres;\n\nALTER TABLE ONLY public.parent\n    ADD CONSTRAINT parent_pkey PRIMARY KEY (pk);\n\nCREATE UNIQUE INDEX c1 ON public.parent USING btree (c1);"), T("CREATE TABLE public.parent (\n    pk bigint NOT NULL,\n    c1 bigint,\n    c2 bigint\n);\n\nALTER TABLE public.parent OWNER TO postgres;\n\nALTER TABLE ONLY public.parent\n    ADD CONSTRAINT parent_pkey PRIMARY KEY (pk);\n\nCREATE UNIQUE INDEX c2 ON public.parent USING btree (c2);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_diff writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: --cached/--staged",
            intent: "STAGED against HEAD shows what is staged and leaves out unstaged changes.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'STAGED');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'STAGED');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.test"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'First commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'STAGED', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk int, c1 int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD', 'STAGED', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
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
            name: "diff: with invalid ref does not panic",
            intent: "A commit hash works as a revision, and a revision that names nothing fails without crashing.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'table');",
                "SELECT set_config('test.first', (SELECT commit_hash FROM dolt_log ORDER BY commit_order LIMIT 1), false);",
                "SELECT * FROM dolt_checkout('-b', 'test-branch');",
                "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'added row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff(current_setting('test.first'), 'test-branch', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main@' || current_setting('test.first'), 'test-branch', 'test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('ref.with.period', 'test-branch', 'test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: ref.with.period", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff(current_setting('test.first') || '..test-branch', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('ref.with.period..test-branch', 'test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: ref.with.period", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff(current_setting('test.first') || '...test-branch', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('ref.with.period...test-branch', 'test');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: ref.with.period", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: binary data in sql output is hex encoded",
            intent: "dolt_patch writes bytea values as hex literals.",
            set_up_script: &[
                "CREATE TABLE t (\"PK\" bytea PRIMARY KEY, c1 bytea);",
                "INSERT INTO t VALUES ('\\xead543', '\\x1ee4'), ('\\x0e', NULL);",
                "SELECT dolt_commit('-Am', 'creating table t');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD~', 'HEAD') WHERE diff_type = 'data' ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("INSERT INTO public.t (\"PK\", c1) VALUES ('\\x0e', NULL);")],
                            &[T("INSERT INTO public.t (\"PK\", c1) VALUES ('\\xead543', '\\x1ee4');")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: with foreign key and sql output",
            intent: "dolt_patch writes a new table's foreign key.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, pv1 int, pv2 int);",
                "CREATE UNIQUE INDEX v1 ON parent (pv1);",
                "CREATE INDEX v2 ON parent (pv2);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'hi');",
                "CREATE TABLE child (id int PRIMARY KEY, cv1 int, cv2 int, CONSTRAINT fk_named FOREIGN KEY (cv1) REFERENCES parent(pv1));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('main', 'WORKING') WHERE diff_type = 'schema' ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.child (\n    id integer NOT NULL,\n    cv1 integer,\n    cv2 integer\n);")],
                            &[T("ALTER TABLE public.child OWNER TO postgres;")],
                            &[T("ALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (id);")],
                            &[T("ALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk_named FOREIGN KEY (cv1) REFERENCES public.parent(pv1);")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("a foreign key needs a unique index on the columns it references in Postgres, and MySQL's index made for a foreign key's own columns has no Postgres counterpart")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: skinny flag only shows row changed without schema changes",
            intent: "dolt_diff's --skinny keeps the key and the columns some row changed.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, val1 int, val2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1, 1);",
                "SELECT dolt_commit('-am', 'cm1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT * FROM dolt_diff('--skinny', 'HEAD~1', 'WORKING', 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bool_or(column_name::text LIKE '%pk') AS has_pk, bool_or(column_name::text LIKE '%val1') AS has_val1, bool_or(column_name::text LIKE '%val2') AS has_val2 FROM information_schema.columns WHERE table_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pk", BOOL), Column("has_val1", BOOL), Column("has_val2", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1 = 2 WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1 = 3 WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT * FROM dolt_diff('--skinny', 'HEAD~1', 'WORKING', 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bool_or(column_name::text LIKE '%pk') AS has_pk, bool_or(column_name::text LIKE '%val1') AS has_val1, bool_or(column_name::text LIKE '%val2') AS has_val2 FROM information_schema.columns WHERE table_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pk", BOOL), Column("has_val1", BOOL), Column("has_val2", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: skinny flag only shows row changed when both schema (column added) and data is changed (row updated)",
            intent: "dolt_diff's --skinny keeps an added column some row set, and leaves out an unchanged one.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, val1 int, val2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1, 1);",
                "INSERT INTO t VALUES (2, 2, 2);",
                "SELECT dolt_commit('-am', 'cm1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT * FROM dolt_diff('--skinny', 'HEAD~1', 'WORKING', 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bool_or(column_name::text LIKE '%pk') AS has_pk, bool_or(column_name::text LIKE '%val1') AS has_val1, bool_or(column_name::text LIKE '%val2') AS has_val2 FROM information_schema.columns WHERE table_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pk", BOOL), Column("has_val1", BOOL), Column("has_val2", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1 = 3 WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD val3 int;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val3 = 4 WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT * FROM dolt_diff('--skinny', 'HEAD~1', 'WORKING', 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bool_or(column_name::text LIKE '%pk') AS has_pk, bool_or(column_name::text LIKE '%val1') AS has_val1, bool_or(column_name::text LIKE '%val3') AS has_val3, bool_or(column_name::text LIKE '%val2') AS has_val2 FROM information_schema.columns WHERE table_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pk", BOOL), Column("has_val1", BOOL), Column("has_val3", BOOL), Column("has_val2", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: skinny flag only shows row changed when both schema (column dropped) and data is changed (row updated)",
            intent: "dolt_diff's --skinny keeps a dropped column, whose values every row lost.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, val1 int, s varchar(255));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1, 'bla');",
                "INSERT INTO t VALUES (2, 2, 'bla2');",
                "SELECT dolt_commit('-am', 'cm1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT * FROM dolt_diff('--skinny', 'HEAD~1', 'WORKING', 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bool_or(column_name::text LIKE '%pk') AS has_pk, bool_or(column_name::text LIKE '%val1') AS has_val1, bool_or(column_name::text LIKE '%s') AS has_s FROM information_schema.columns WHERE table_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pk", BOOL), Column("has_val1", BOOL), Column("has_s", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP COLUMN s;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1 = 3 WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1 = 4 WHERE pk = 2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT * FROM dolt_diff('--skinny', 'HEAD~1', 'WORKING', 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bool_or(column_name::text LIKE '%pk') AS has_pk, bool_or(column_name::text LIKE '%val1') AS has_val1, bool_or(column_name::text LIKE '%s') AS has_s FROM information_schema.columns WHERE table_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pk", BOOL), Column("has_val1", BOOL), Column("has_s", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: skinny flag only shows row changed when data is changed (row deleted)",
            intent: "dolt_diff's --skinny keeps every column of a deleted row.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, val1 int, val2 int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1, 1);",
                "INSERT INTO t VALUES (2, 2, 2);",
                "SELECT dolt_commit('-am', 'cm1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT * FROM dolt_diff('--skinny', 'HEAD~1', 'WORKING', 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bool_or(column_name::text LIKE '%pk') AS has_pk, bool_or(column_name::text LIKE '%val1') AS has_val1, bool_or(column_name::text LIKE '%val2') AS has_val2 FROM information_schema.columns WHERE table_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pk", BOOL), Column("has_val1", BOOL), Column("has_val2", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT * FROM dolt_diff('--skinny', 'HEAD~1', 'WORKING', 't');",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT bool_or(column_name::text LIKE '%pk') AS has_pk, bool_or(column_name::text LIKE '%val1') AS has_val1, bool_or(column_name::text LIKE '%val2') AS has_val2 FROM information_schema.columns WHERE table_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("has_pk", BOOL), Column("has_val1", BOOL), Column("has_val2", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW v;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: keyless sql diffs",
            intent: "dolt_patch writes a keyless table's changes as a delete or insert per row, duplicates included, and its key changes alone.",
            set_up_script: &[
                "CREATE TABLE t (pk int, val int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm1');",
                "INSERT INTO t VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("INSERT INTO public.t (pk, val) VALUES (1, 1);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("INSERT INTO public.t (pk, val) VALUES (1, 1);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val = 2 WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.t WHERE pk = 1 AND val = 1;")],
                            &[T("DELETE FROM public.t WHERE pk = 1 AND val = 1;")],
                            &[T("INSERT INTO public.t (pk, val) VALUES (1, 2);")],
                            &[T("INSERT INTO public.t (pk, val) VALUES (1, 2);")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm4');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE val < 3;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("DELETE FROM public.t WHERE pk = 1 AND val = 2;")],
                            &[T("DELETE FROM public.t WHERE pk = 1 AND val = 2;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm5');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\", and dolt_patch writes the rows of a table whose primary key changed, deleting the rows it changes by the old key before the definition changes and inserting the new rows after them, where Dolt warns and leaves them out, Appendix C \"dolt_patch of a table whose primary key changed\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm6');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN pk2 int;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk, val);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t ADD COLUMN pk2 integer;")],
                            &[T("ALTER TABLE ONLY public.t DROP CONSTRAINT t_pkey;")],
                            &[T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk, val);")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\", and dolt_patch writes the rows of a table whose primary key changed, deleting the rows it changes by the old key before the definition changes and inserting the new rows after them, where Dolt warns and leaves them out, Appendix C \"dolt_patch of a table whose primary key changed\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: adding and removing primary key",
            intent: "dolt_patch writes a primary key's addition and removal and warns that it leaves out the rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int, val int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table');",
                "ALTER TABLE t ADD PRIMARY KEY (pk);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\", and dolt_patch writes the rows of a table whose primary key changed, deleting the rows it changes by the old key before the definition changes and inserting the new rows after them, where Dolt warns and leaves them out, Appendix C \"dolt_patch of a table whose primary key changed\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'added primary key');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE ONLY public.t DROP CONSTRAINT t_pkey;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\", and dolt_patch writes the rows of a table whose primary key changed, deleting the rows it changes by the old key before the definition changes and inserting the new rows after them, where Dolt warns and leaves them out, Appendix C \"dolt_patch of a table whose primary key changed\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: created and dropped tables include schema and data changes in results",
            intent: "A created or dropped table shows as added or dropped with its rows.",
            set_up_script: &[
                "CREATE TABLE a (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'creating table a');",
                "INSERT INTO a VALUES (1), (2);",
                "SELECT dolt_commit('-am', 'inserting data into table a');",
                "DROP TABLE a;",
                "SELECT dolt_commit('-am', 'dropping table a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~3', 'HEAD~2', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.a"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~3', 'HEAD~1', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.a"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD~3', 'HEAD~1', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("added")],
                            &[T("2"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.a"), T(""), T("dropped"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD~1', 'HEAD', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("removed")],
                            &[T("2"), T("removed")],
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
            name: "diff: large diff does not drop rows",
            intent: "A diff of a thousand updated rows returns all of them.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO t SELECT i, i FROM generate_series(1, 1000) i;",
                "SELECT dolt_commit('-am', 'Add the initial rows');",
                "UPDATE t SET val = val + 1 WHERE pk < 10000;",
                "SELECT dolt_commit('-am', 'make a bulk update creating a large diff');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff('HEAD~1', 'WORKING', 't') WHERE dolt_diff_type = 'modified';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1000")],
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
            name: "diff: works with spaces in column names",
            intent: "dolt_diff names the diff columns of a column with spaces in its name.",
            set_up_script: &[
                "CREATE TABLE t (pk int, \"type of food\" varchar(100));",
                "INSERT INTO t VALUES (1, 'ramen');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, \"to_type of food\", dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_type of food", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("ramen"), T("added")],
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
            name: "diff: allowed across primary key renames",
            intent: "Renamed key columns still match rows, so a changed row shows as modified.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, col1 int);",
                "INSERT INTO t1 VALUES (1, 1);",
                "CREATE TABLE t2 (pk1a int, pk1b int, col1 int, PRIMARY KEY (pk1a, pk1b));",
                "INSERT INTO t2 VALUES (1, 1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial');",
                "ALTER TABLE t1 RENAME COLUMN pk TO pk2;",
                "UPDATE t1 SET col1 = 100;",
                "ALTER TABLE t2 RENAME COLUMN pk1a TO pk2a;",
                "ALTER TABLE t2 RENAME COLUMN pk1b TO pk2b;",
                "UPDATE t2 SET col1 = 100;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'rename primary key');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_col1, to_pk2, to_col1, dolt_diff_type FROM dolt_diff('HEAD~1', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_col1", INT4), Column("to_pk2", INT4), Column("to_col1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("100"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk1a, from_pk1b, from_col1, to_pk2a, to_pk2b, to_col1, dolt_diff_type FROM dolt_diff('HEAD~1', 'HEAD', 't2');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk1a", INT4), Column("from_pk1b", INT4), Column("from_col1", INT4), Column("to_pk2a", INT4), Column("to_pk2b", INT4), Column("to_col1", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("1"), T("1"), T("100"), T("modified")],
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
            name: "diff: add a column, then set and unset its value. Should not show a diff",
            intent: "Setting a value of a column added later and setting it back leaves no diff and no stat.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1), (2), (3);",
                "ALTER TABLE t ADD COLUMN col1 int;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'setup');",
                "UPDATE t SET col1 = 1 WHERE pk = 1;",
                "UPDATE t SET col1 = NULL WHERE pk = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('STAGED', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('STAGED', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
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
            name: "diff: schema change int to float",
            intent: "A row diff across a column's type change shows each side's value in its own type.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, val int);",
                "INSERT INTO t1 VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'commit');",
                "ALTER TABLE t1 ALTER COLUMN val TYPE float;",
                "UPDATE t1 SET val = val + 0.234;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_val, to_pk, to_val, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_val", INT4), Column("to_pk", INT4), Column("to_val", FLOAT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("1.234"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_val, to_val, dolt_diff_type FROM dolt_diff('WORKING', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_val", FLOAT8), Column("to_val", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1.234"), T("1"), T("modified")],
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
            name: "diff: schema change int to decimal",
            intent: "A row diff across a column's type change shows each side's value in its own type.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, val int);",
                "INSERT INTO t1 VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'commit');",
                "ALTER TABLE t1 ALTER COLUMN val TYPE decimal(20,4);",
                "UPDATE t1 SET val = val + 0.234;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_val, to_pk, to_val, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_val", INT4), Column("to_pk", INT4), Column("to_val", NUMERIC), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("1.2340"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_val, to_val, dolt_diff_type FROM dolt_diff('WORKING', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_val", NUMERIC), Column("to_val", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1.2340"), T("1"), T("modified")],
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
            name: "diff: schema change float to double",
            intent: "A row diff across a column's type change shows each side's value in its own type.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, val real);",
                "INSERT INTO t1 VALUES (1, 16777217);",
                "SELECT dolt_commit('-Am', 'commit');",
                "ALTER TABLE t1 ALTER COLUMN val TYPE double precision;",
                "UPDATE t1 SET val = 16777217;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_val, to_pk, to_val, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_val", FLOAT4), Column("to_pk", INT4), Column("to_val", FLOAT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1.6777216e+07"), T("1"), T("16777217"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's float is 4 bytes, which Postgres calls real")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_val, to_val, dolt_diff_type FROM dolt_diff('WORKING', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_val", FLOAT8), Column("to_val", FLOAT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("16777217"), T("1.6777216e+07"), T("modified")],
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
            name: "diff: schema change int to bigint",
            intent: "A row diff across a column's type change shows each side's value in its own type.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, val smallint);",
                "INSERT INTO t1 VALUES (1, 32760);",
                "SELECT dolt_commit('-Am', 'commit');",
                "ALTER TABLE t1 ALTER COLUMN val TYPE bigint;",
                "UPDATE t1 SET val = val + 10000;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_val, to_pk, to_val, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_val", INT2), Column("to_pk", INT4), Column("to_val", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("32760"), T("1"), T("42760"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_val, to_val, dolt_diff_type FROM dolt_diff('WORKING', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_val", INT8), Column("to_val", INT2), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("42760"), T("32760"), T("modified")],
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
            name: "diff: schema change DATE to TIMESTAMP",
            intent: "A row diff across a column's type change shows each side's value in its own type.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, val date);",
                "INSERT INTO t1 VALUES (1, '2023-01-01');",
                "SELECT dolt_commit('-Am', 'commit');",
                "ALTER TABLE t1 ALTER COLUMN val TYPE timestamp;",
                "UPDATE t1 SET val = '2023-01-01 12:21:42';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_val, to_pk, to_val, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_val", DATE), Column("to_pk", INT4), Column("to_val", TIMESTAMP), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("2023-01-01"), T("1"), T("2023-01-01 12:21:42"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_val, to_val, dolt_diff_type FROM dolt_diff('WORKING', 'HEAD', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_val", TIMESTAMP), Column("to_val", DATE), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("2023-01-01 12:21:42"), T("2023-01-01"), T("modified")],
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
            name: "diff: schema change increase decimal precision",
            intent: "A keyless row whose stored value changed with its column's scale shows as removed and added.",
            set_up_script: &[
                "CREATE TABLE t1 (d decimal(10, 2));",
                "INSERT INTO t1 VALUES (123.45);",
                "SELECT dolt_commit('-Am', 'commit');",
                "ALTER TABLE t1 ALTER COLUMN d TYPE decimal(10, 4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_d, to_d, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_d", NUMERIC), Column("to_d", NUMERIC), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("123.45"), Null, T("removed")],
                            &[Null, T("123.4500"), T("added")],
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
            name: "diff: schema change decrease decimal precision",
            intent: "A keyless row whose stored value changed with its column's scale shows as removed and added.",
            set_up_script: &[
                "CREATE TABLE t1 (d decimal(10, 4));",
                "INSERT INTO t1 VALUES (123.4567);",
                "SELECT dolt_commit('-Am', 'commit');",
                "ALTER TABLE t1 ALTER COLUMN d TYPE decimal(10, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_d, to_d, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't1');",
                    expected: Expected::Rows {
                        columns: &[Column("from_d", NUMERIC), Column("to_d", NUMERIC), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("123.4567"), Null, T("removed")],
                            &[Null, T("123.46"), T("added")],
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
            name: "diff: duplicate commit_hash in diff",
            intent: "The dolt_diff table has a row for each table one commit made.",
            set_up_script: &[
                "CREATE TABLE t1 (i int PRIMARY KEY);",
                "CREATE TABLE t2 (j int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit 1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff WHERE commit_hash = dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t1")],
                            &[T("public.t2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: enum data change",
            intent: "A diff shows an enum column's old and new labels.",
            set_up_script: &[
                "CREATE TYPE size_enum AS ENUM ('x-small', 'small', 'medium', 'large', 'x-large');",
                "CREATE TABLE test (pk int PRIMARY KEY, size size_enum);",
                "INSERT INTO test VALUES (1, 'x-small');",
                "INSERT INTO test VALUES (2, 'small');",
                "INSERT INTO test VALUES (3, 'medium');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'First commit');",
                "INSERT INTO test VALUES (4, 'large');",
                "DELETE FROM test WHERE pk = 1;",
                "UPDATE test SET size = 'x-large' WHERE pk = 2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, from_size, to_size, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("from_size", USER_DEFINED), Column("to_size", USER_DEFINED), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("x-small"), Null, T("removed")],
                            &[T("2"), T("small"), T("x-large"), T("modified")],
                            &[T("4"), Null, T("large"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's inline ENUM is a Postgres enum type here")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: enum and schema changes",
            intent: "A diff shows a column added beside an enum column, and the enum column's changed labels.",
            set_up_script: &[
                "CREATE TYPE size_enum AS ENUM ('x-small', 'small', 'medium', 'large', 'x-large');",
                "CREATE TABLE test (pk int PRIMARY KEY, size size_enum);",
                "INSERT INTO test VALUES (1, 'x-small');",
                "INSERT INTO test VALUES (2, 'small');",
                "INSERT INTO test VALUES (3, 'medium');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'First commit');",
                "ALTER TABLE test ADD COLUMN c1 int;",
                "INSERT INTO test VALUES (4, 'large', 1);",
                "DELETE FROM test WHERE pk = 1;",
                "UPDATE test SET size = 'x-large' WHERE pk = 2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("CREATE TABLE public.test (\n    pk integer NOT NULL,\n    size public.size_enum\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.test (\n    pk integer NOT NULL,\n    size public.size_enum,\n    c1 integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (pk);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's inline ENUM is a Postgres enum type here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, from_size, to_size, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("from_size", USER_DEFINED), Column("to_size", USER_DEFINED), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("x-small"), Null, T("removed")],
                            &[T("2"), T("small"), T("x-large"), T("modified")],
                            &[T("4"), Null, T("large"), T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's inline ENUM is a Postgres enum type here")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: autoincrement registers as schema diff",
            intent: "Advancing a table's identity sequence shows as a change even when no row changed.",
            set_up_script: &[
                "CREATE TABLE test (pk int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY);",
                "INSERT INTO test DEFAULT VALUES;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'First commit');",
                "INSERT INTO test DEFAULT VALUES;",
                "DELETE FROM test WHERE pk = 2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.test"), T("public.test"), T("modified"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: -r sql returns error when data statements cannot be generated",
            intent: "dolt_patch writes a column's type change, rename, addition, or drop and then the rows, so the rows read back the same, setting an added column whose default differs from the row's value.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, val1 int, val2 varchar(10));",
                "SELECT dolt_add('.');",
                "INSERT INTO t VALUES (1, 1, '1');",
                "SELECT dolt_commit('-am', 'cm1');",
                "SELECT dolt_branch('old');",
                "UPDATE t SET val2 = '2';",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'cm2');",
                "SELECT dolt_branch('start');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'start');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN val2 TYPE varchar(20);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'change');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('old', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t ALTER COLUMN val2 TYPE character varying(20);")],
                            &[T("UPDATE public.t SET val2 = '2' WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'start');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN val2 TYPE varchar(5);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'change');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('old', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t ALTER COLUMN val2 TYPE character varying(5);")],
                            &[T("UPDATE public.t SET val2 = '2' WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'start');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN val2 TYPE text;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'change');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('old', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t ALTER COLUMN val2 TYPE text;")],
                            &[T("UPDATE public.t SET val2 = '2' WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'start');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t RENAME COLUMN val2 TO val3;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'change');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('old', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t RENAME COLUMN val2 TO val3;")],
                            &[T("UPDATE public.t SET val3 = '2' WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'start');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN val3 varchar(10) DEFAULT '3'; UPDATE t SET val3 = NULL;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'change');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('old', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t ADD COLUMN val3 character varying(10) DEFAULT '3'::character varying;")],
                            &[T("UPDATE public.t SET val2 = '2', val3 = NULL WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'start');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP COLUMN val1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'change');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('old', 'WORKING') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.t DROP COLUMN val1;")],
                            &[T("UPDATE public.t SET val2 = '2' WHERE pk = 1;")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt_patch writes Postgres statements in pg_dump's text, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: A...B with several merge bases uses the newest and warns",
            intent: "A three-dot range with two merge bases diffs from the newest one and warns.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'branchA');",
                "INSERT INTO t VALUES (10);",
                "SELECT set_config('test.base_a', dolt_commit('-am', 'baseA'), false);",
                "SELECT * FROM dolt_checkout('-b', 'branchB', 'main');",
                "INSERT INTO t VALUES (20);",
                "SELECT pg_sleep(1);",
                "SELECT set_config('test.base_b', dolt_commit('-am', 'baseB'), false);",
                "SELECT dolt_merge(current_setting('test.base_a'), '-m', 'mergeB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT dolt_merge(current_setting('test.base_b'), '-m', 'mergeA');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('branchA...branchB', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("10"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "multiple merge bases, using", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: ...B diffs B against the merge base of HEAD and B",
            intent: "A three-dot range from HEAD diffs from the newest merge base and warns.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'branchA');",
                "INSERT INTO t VALUES (10);",
                "SELECT set_config('test.base_a', dolt_commit('-am', 'baseA'), false);",
                "SELECT * FROM dolt_checkout('-b', 'branchB', 'main');",
                "INSERT INTO t VALUES (20);",
                "SELECT pg_sleep(1);",
                "SELECT set_config('test.base_b', dolt_commit('-am', 'baseB'), false);",
                "SELECT dolt_merge(current_setting('test.base_a'), '-m', 'mergeB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT dolt_merge(current_setting('test.base_b'), '-m', 'mergeA');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('HEAD...branchB', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("10"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "multiple merge bases, using", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: A...B with one merge base does not warn",
            intent: "A three-dot range with one merge base doesn't warn.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'branchA');",
                "INSERT INTO t VALUES (10);",
                "SELECT set_config('test.base_a', dolt_commit('-am', 'baseA'), false);",
                "SELECT * FROM dolt_checkout('-b', 'branchB', 'main');",
                "INSERT INTO t VALUES (20);",
                "SELECT pg_sleep(1);",
                "SELECT set_config('test.base_b', dolt_commit('-am', 'baseB'), false);",
                "SELECT dolt_merge(current_setting('test.base_a'), '-m', 'mergeB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT dolt_merge(current_setting('test.base_b'), '-m', 'mergeA');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COALESCE(to_pk, from_pk) AS pk, dolt_diff_type FROM dolt_diff('main...branchA', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("10"), T("added")],
                            &[T("20"), T("added")],
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
            name: "diff: new foreign key added and resolves",
            intent: "The working set's schema diff shows a new table's foreign key.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE parent (i int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE child (j int PRIMARY KEY, FOREIGN KEY (j) REFERENCES parent (i));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'child');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("CREATE TABLE public.child (\n    j integer NOT NULL\n);\n\nALTER TABLE public.child OWNER TO postgres;\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (j);\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_j_fkey FOREIGN KEY (j) REFERENCES public.parent(i);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: existing foreign key that was resolved is deleted",
            intent: "A dropped foreign key shows as removed in the schema diff.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE parent (i int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE child (j int PRIMARY KEY, CONSTRAINT fk FOREIGN KEY (j) REFERENCES parent (i));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'child');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("CREATE TABLE public.child (\n    j integer NOT NULL\n);\n\nALTER TABLE public.child OWNER TO postgres;\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (j);\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk FOREIGN KEY (j) REFERENCES public.parent(i);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'init commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'child');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.child (\n    j integer NOT NULL\n);\n\nALTER TABLE public.child OWNER TO postgres;\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (j);\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk FOREIGN KEY (j) REFERENCES public.parent(i);"), T("CREATE TABLE public.child (\n    j integer NOT NULL\n);\n\nALTER TABLE public.child OWNER TO postgres;\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (j);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: existing foreign key that was resolved is modified",
            intent: "A foreign key recreated on a renamed column shows the old and new keys in the schema diff.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE parent (i int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE child (j int PRIMARY KEY, CONSTRAINT fk FOREIGN KEY (j) REFERENCES parent (i));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'child');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("CREATE TABLE public.child (\n    j integer NOT NULL\n);\n\nALTER TABLE public.child OWNER TO postgres;\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (j);\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk FOREIGN KEY (j) REFERENCES public.parent(i);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'init commit');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT fk;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child RENAME COLUMN j TO k;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child ADD CONSTRAINT fk FOREIGN KEY (k) REFERENCES parent (i);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'child');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.child (\n    j integer NOT NULL\n);\n\nALTER TABLE public.child OWNER TO postgres;\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (j);\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk FOREIGN KEY (j) REFERENCES public.parent(i);"), T("CREATE TABLE public.child (\n    k integer CONSTRAINT child_j_not_null NOT NULL\n);\n\nALTER TABLE public.child OWNER TO postgres;\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT child_pkey PRIMARY KEY (k);\n\nALTER TABLE ONLY public.child\n    ADD CONSTRAINT fk FOREIGN KEY (k) REFERENCES public.parent(i);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: with where clause",
            intent: "A diff filtered by the from and to columns keeps only the matching rows, and filtering on a bare column fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'table created');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 222, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 333, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_c1 FROM dolt_diff('HEAD', 'WORKING', 'test') WHERE to_pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("to_c1", INT8)],
                        rows: &[
                            &[T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_c1 FROM dolt_diff('HEAD', 'WORKING', 'test') WHERE to_pk < 3;",
                    expected: Expected::Rows {
                        columns: &[Column("to_c1", INT8)],
                        rows: &[
                            &[T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added two rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4, 44, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'committed to branch test1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (5, 55, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'committed to branch test2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1', 'test2', 'test') ORDER BY from_pk, to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c1", INT8), Column("to_c1", INT8)],
                        rows: &[
                            &[T("44"), Null],
                            &[Null, T("55")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the diff, which dolt_diff() returns as rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1', 'test2', 'test') WHERE from_pk = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c1", INT8), Column("to_c1", INT8)],
                        rows: &[
                            &[T("44"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1', 'test2', 'test') WHERE from_pk = 4 OR to_pk = 5 ORDER BY from_pk, to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c1", INT8), Column("to_c1", INT8)],
                        rows: &[
                            &[T("44"), Null],
                            &[Null, T("55")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1', 'test2', 'test') WHERE from_pk = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c1", INT8), Column("to_c1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1', 'test2', 'test') WHERE to_pk = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c1", INT8), Column("to_c1", INT8)],
                        rows: &[
                            &[Null, T("55")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1', 'test2', 'test') WHERE to_pk = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c1", INT8), Column("to_c1", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1', 'test2', 'test') WHERE pk = 4;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"pk\" does not exist", position: 70, ..E }),
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1..test2', 'test') ORDER BY from_pk, to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c1", INT8), Column("to_c1", INT8)],
                        rows: &[
                            &[T("44"), Null],
                            &[Null, T("55")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the diff, which dolt_diff() returns as rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1..test2', 'test') WHERE from_pk = 4 OR to_pk = 5 ORDER BY from_pk, to_pk;",
                    expected: Expected::Rows {
                        columns: &[Column("from_c1", INT8), Column("to_c1", INT8)],
                        rows: &[
                            &[T("44"), Null],
                            &[Null, T("55")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, to_c1 FROM dolt_diff('test1..test2', 'test') WHERE pk = 4;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"pk\" does not exist", position: 68, ..E }),
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: with where clause errors",
            intent: "A diff filtered by a malformed condition or a missing column fails.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 0, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1, 1, 1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'table created');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 22, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 33, 0, 0, 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD', 'WORKING', 'test') WHERE some nonsense;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"some\"", position: 58, ..E }),
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD', 'WORKING', 'test') WHERE poop = 0;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"poop\" does not exist", position: 58, ..E }),
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'added two rows');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD', 'WORKING', 'test') WHERE poop = 0;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"poop\" does not exist", position: 58, ..E }),
                    dolt: Some(Dolt::Differs("dolt diff --where filters the diff's rows, which a WHERE on dolt_diff() does")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: table with same name on different branches with different primary key sets",
            intent: "A table whose primary key differs between branches diffs its schema, and its data diff is refused.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('another-branch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a (id int PRIMARY KEY, pv1 int, pv2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'hi');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('another-branch');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a (id int, cv1 int, cv2 int, PRIMARY KEY (id, cv1));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('-A');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'hello');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('main', 'another-branch', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.a (\n    id integer NOT NULL,\n    pv1 integer,\n    pv2 integer\n);\n\nALTER TABLE public.a OWNER TO postgres;\n\nALTER TABLE ONLY public.a\n    ADD CONSTRAINT a_pkey PRIMARY KEY (id);"), T("CREATE TABLE public.a (\n    id integer NOT NULL,\n    cv1 integer NOT NULL,\n    cv2 integer\n);\n\nALTER TABLE public.a OWNER TO postgres;\n\nALTER TABLE ONLY public.a\n    ADD CONSTRAINT a_pkey PRIMARY KEY (id, cv1);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main', 'another-branch', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("to_cv1", INT4), Column("to_cv2", INT4), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_id", INT4), Column("from_pv1", INT4), Column("from_pv2", INT4), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints that the primary key sets differ and skips the data diff, where dolt_diff() returns no rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('main..another-branch', 'a');",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("to_cv1", INT4), Column("to_cv2", INT4), Column("dolt_to_commit", TEXT), Column("dolt_to_commit_date", TIMESTAMPTZ), Column("from_id", INT4), Column("from_pv1", INT4), Column("from_pv2", INT4), Column("dolt_from_commit", TEXT), Column("dolt_from_commit_date", TIMESTAMPTZ), Column("dolt_diff_type", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints that the primary key sets differ and skips the data diff, where dolt_diff() returns no rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: sql update queries only show changed columns",
            intent: "A diff as SQL updates only the columns that changed.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, val1 int, val2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1=2 WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 't') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("UPDATE public.t SET val1 = 2 WHERE pk = 1;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff -r sql prints the diff as SQL, which dolt_patch() returns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1=3, val2=4 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 't') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("UPDATE public.t SET val1 = 3, val2 = 4 WHERE pk = 1;")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1=3 WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 't') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN val3 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm4');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET val1=30, val3=4 WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch('HEAD', 'WORKING', 't') ORDER BY statement_order;",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("UPDATE public.t SET val1 = 30, val3 = 4 WHERE pk = 1;")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: table-only option",
            intent: "The diff summary lists each changed table with its diff type and whether its data and schema changed.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'commit 1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ADD COLUMN j int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t4 (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_table_name, to_table_name, diff_type, data_change, schema_change FROM dolt_diff_summary('HEAD', 'WORKING') ORDER BY coalesce(to_table_name, from_table_name);",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("public.t1"), T(""), T("dropped"), T("f"), T("t")],
                            &[T("public.t2"), T("public.t2"), T("modified"), T("f"), T("t")],
                            &[T("public.t3"), T("public.t3"), T("modified"), T("t"), T("f")],
                            &[T(""), T("public.t4"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("dolt diff --summary prints the summary, which dolt_diff_summary() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: diff --reverse",
            intent: "A diff from the working set back to HEAD shows a new table as dropped.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT from_table_name, to_table_name, diff_type FROM dolt_diff_summary('WORKING', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.test"), T(""), T("dropped")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff -R reverses the diff, which swapping dolt_diff_summary()'s revisions does")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: dolt_tests system table changes should show in dolt diff and dolt status",
            intent: "Rows added to and changed in dolt_tests show in the status and the diff.",
            set_up_script: &[
                "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('test1', 'description', 'SELECT 1', 'expected_rows', '==', '1');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_tests"), T("f"), T("new table")],
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_table_name, diff_type FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("dolt_tests"), T("added")],
                            &[T("public.test"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'Added test to dolt_tests');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE dolt_tests SET test_group = 'updated description' WHERE test_name = 'test1';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_tests"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_test_group FROM dolt_diff('HEAD', 'WORKING', 'dolt_tests');",
                    expected: Expected::Rows {
                        columns: &[Column("to_test_group", TEXT)],
                        rows: &[
                            &[T("updated description")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "diff: --merge-base with several merge bases fails",
            intent: "A three-dot diff of branches with two merge bases warns and diffs from Dolt's first merge base.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-m', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'branchA');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'baseA');",
                "SELECT set_config('test.base_a', dolt_hashof('HEAD'), FALSE);",
                "SELECT * FROM dolt_checkout('-b', 'branchB', 'main');",
                "INSERT INTO t VALUES (20);",
                "SELECT pg_sleep(1);",
                "SELECT dolt_commit('-am', 'baseB');",
                "SELECT set_config('test.base_b', dolt_hashof('HEAD'), FALSE);",
                "SELECT * FROM dolt_merge(current_setting('test.base_a'), '-m', 'mergeB');",
                "SELECT * FROM dolt_checkout('branchA');",
                "SELECT * FROM dolt_merge(current_setting('test.base_b'), '-m', 'mergeA');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, from_pk, dolt_diff_type FROM dolt_diff('branchA...branchB', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("from_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("10"), Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "branchA...branchB: multiple merge bases, using ", message_contains: true, ..N }],
                    dolt: Some(Dolt::Differs("The bats check is dolt diff --merge-base's refusal of several merge bases, where Dolt's dolt_diff() warns and diffs from the first, as Doltgrust's does. Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
