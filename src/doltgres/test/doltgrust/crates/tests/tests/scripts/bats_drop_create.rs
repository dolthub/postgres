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

//! Dolt's integration-tests/bats/drop-create.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_drop_create() {
    run_scripts(&[
        ScriptTest {
            name: r#"drop-create: same schema and data"#,
            intent: r#"Dropping a table and creating it again with the same definition and rows leaves the working set clean."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: same schema and data, commit after drop"#,
            intent: r#"A table committed as dropped and created again with its old definition and rows is a new table to the head, and no change at all against the commit before the drop."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
                r#"SELECT dolt_commit('-am', 'deleted table');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_diff_summary('HEAD~', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff HEAD~ is a CLI command, so the port reads dolt_diff_summary()")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: added column"#,
            intent: r#"A table dropped and created again with one more column and the same rows is a modified table whose only change is the new column."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL, c int NULL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test(a,b) VALUES (1,1), (2,2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    a integer NOT NULL,\n    b integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);"), T("CREATE TABLE public.test (\n    a integer NOT NULL,\n    b integer,\n    c integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff is a CLI command, so the port reads dolt_schema_diff(), whose statements are pg_dump's for Postgres' types (tinyint becomes smallint), where Dolt's are MySQL's SHOW CREATE TABLE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_diff_type, from_a, from_b, to_a, to_b, to_c FROM dolt_diff('HEAD', 'WORKING', 'test') ORDER BY coalesce(to_a, from_a);"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_a", INT4), Column("from_b", INT4), Column("to_a", INT4), Column("to_b", INT4), Column("to_c", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff() for the rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: added column with data modifications"#,
            intent: r#"A table dropped and created again with one more column and changed rows shows the new column and each changed and added row."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL, c int NULL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test(a,b,c) VALUES (1,2,1), (2,3,2), (3,3,3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_diff_type, from_a, from_b, to_a, to_b, to_c FROM dolt_diff('HEAD', 'WORKING', 'test') ORDER BY coalesce(to_a, from_a);"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_a", INT4), Column("from_b", INT4), Column("to_a", INT4), Column("to_b", INT4), Column("to_c", INT4)],
                        rows: &[
                            &[T("modified"), T("1"), T("1"), T("1"), T("2"), T("1")],
                            &[T("modified"), T("2"), T("2"), T("2"), T("3"), T("2")],
                            &[T("added"), Null, Null, T("3"), T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff() for the rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: dropped column"#,
            intent: r#"A table dropped and created again without a column shows the column gone and each row that lost its value."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL, c int NULL);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test VALUES (1,2,3), (4,5,6);"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test(a,b) VALUES (1,2), (4,5);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    a integer NOT NULL,\n    b integer,\n    c integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);"), T("CREATE TABLE public.test (\n    a integer NOT NULL,\n    b integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff is a CLI command, so the port reads dolt_schema_diff(), whose statements are pg_dump's for Postgres' types (tinyint becomes smallint), where Dolt's are MySQL's SHOW CREATE TABLE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_diff_type, from_a, from_b, from_c, to_a, to_b FROM dolt_diff('HEAD', 'WORKING', 'test') ORDER BY coalesce(to_a, from_a);"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_a", INT4), Column("from_b", INT4), Column("from_c", INT4), Column("to_a", INT4), Column("to_b", INT4)],
                        rows: &[
                            &[T("modified"), T("1"), T("2"), T("3"), T("1"), T("2")],
                            &[T("modified"), T("4"), T("5"), T("6"), T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff() for the rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: dropped column with data modifications"#,
            intent: r#"A table dropped and created again without a column and with changed rows shows each changed and added row."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL, c int NULL);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test VALUES (1,2,3), (4,5,6);"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test(a,b) VALUES (1,7), (4,8), (9,10);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_diff_type, from_a, from_b, from_c, to_a, to_b FROM dolt_diff('HEAD', 'WORKING', 'test') ORDER BY coalesce(to_a, from_a);"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_a", INT4), Column("from_b", INT4), Column("from_c", INT4), Column("to_a", INT4), Column("to_b", INT4)],
                        rows: &[
                            &[T("modified"), T("1"), T("2"), T("3"), T("1"), T("7")],
                            &[T("modified"), T("4"), T("5"), T("6"), T("4"), T("8")],
                            &[T("added"), Null, Null, Null, T("9"), T("10")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff() for the rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: added column, modified column"#,
            intent: r#"A table dropped and created again with retyped columns and a new one shows each column's change."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a bigint PRIMARY KEY, b smallint NULL, c int NULL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("tinyint is MySQL's, so the port uses smallint")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test(a,b) VALUES (1,1), (2,2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    a integer NOT NULL,\n    b integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);"), T("CREATE TABLE public.test (\n    a bigint NOT NULL,\n    b smallint,\n    c integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff is a CLI command, so the port reads dolt_schema_diff(), whose statements are pg_dump's for Postgres' types (tinyint becomes smallint), where Dolt's are MySQL's SHOW CREATE TABLE")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: constraint changes"#,
            intent: r#"A table dropped and created again with retyped columns, a NOT NULL, a CHECK constraint, and a new column shows each change."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a bigint PRIMARY KEY, b smallint NOT NULL CHECK (b > 0), c varchar(10));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("tinyint is MySQL's, so the port uses smallint")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test(a,b) VALUES (1,1), (2,2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    a integer NOT NULL,\n    b integer\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);"), T("CREATE TABLE public.test (\n    a bigint NOT NULL,\n    b smallint NOT NULL,\n    c character varying(10),\n    CONSTRAINT test_b_check CHECK ((b > 0))\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff is a CLI command, so the port reads dolt_schema_diff(), whose statements are pg_dump's for Postgres' types (tinyint becomes smallint), where Dolt's are MySQL's SHOW CREATE TABLE")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: default changes"#,
            intent: r#"A table dropped and created again with other defaults shows each column's change."#,
            set_up_script: &[
                r#"CREATE TABLE test(a int PRIMARY KEY, b int NULL DEFAULT 10);"#,
                r#"SELECT dolt_add('.');"#,
                r#"INSERT INTO test VALUES (1,1), (2,2);"#,
                r#"SELECT dolt_commit('-am', 'table with two rows');"#,
                r#"DROP TABLE test;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test(a bigint PRIMARY KEY, b smallint NOT NULL DEFAULT 50, c varchar(10));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("tinyint is MySQL's, so the port uses smallint")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test(a,b) VALUES (1,1), (2,2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("show tables is MySQL's, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name, staged, status FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt status is a CLI command, so the port reads dolt_status")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.test (\n    a integer NOT NULL,\n    b integer DEFAULT 10\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);"), T("CREATE TABLE public.test (\n    a bigint NOT NULL,\n    b smallint DEFAULT 50 NOT NULL,\n    c character varying(10)\n);\n\nALTER TABLE public.test OWNER TO postgres;\n\nALTER TABLE ONLY public.test\n    ADD CONSTRAINT test_pkey PRIMARY KEY (a);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff is a CLI command, so the port reads dolt_schema_diff(), whose statements are pg_dump's for Postgres' types (tinyint becomes smallint), where Dolt's are MySQL's SHOW CREATE TABLE")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"drop-create: drop table from different database"#,
            intent: r#"Dropping a schema's table of the same name leaves the other schema's table alone."#,
            set_up_script: &[
                r#"CREATE TABLE test (currentId int PRIMARY KEY, currentText text);"#,
                r#"INSERT INTO test VALUES (1, 'text1'), (2, 'text2');"#,
                r#"CREATE SCHEMA common;"#,
                r#"CREATE TABLE common.test (commonId integer, commonText text);"#,
                r#"INSERT INTO common.test VALUES (999, 'common database text1');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM common.test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("commonid", INT4), Column("commontext", TEXT)],
                        rows: &[
                            &[T("999"), T("common database text1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE common.test;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("currentid", INT4), Column("currenttext", TEXT)],
                        rows: &[
                            &[T("1"), T("text1")],
                            &[T("2"), T("text2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM common.test;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "common.test" does not exist"#, position: 15, ..E }),
                    dolt: Some(Dolt::Differs("Postgres names the missing relation with its schema, where Dolt's error is table not found: test")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
