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

//! Dolt's integration-tests/bats/2pk5cols-ints.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_2pk5cols_ints() {
    run_scripts(&[
        ScriptTest {
            name: r#"2pk5cols-ints: empty table"#,
            intent: r#"A new empty table with a two-column key is listed, has no rows, shows as an added table in the diff, and is untracked."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2)); COMMENT ON COLUMN test.pk1 IS 'tag:0'; COMMENT ON COLUMN test.pk2 IS 'tag:1'; COMMENT ON COLUMN test.c1 IS 'tag:2'; COMMENT ON COLUMN test.c2 IS 'tag:3'; COMMENT ON COLUMN test.c3 IS 'tag:4'; COMMENT ON COLUMN test.c4 IS 'tag:5'; COMMENT ON COLUMN test.c5 IS 'tag:6';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is a CLI command, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT to_table_name, diff_type FROM dolt_diff_summary('HEAD', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.test"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff_summary")),
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
            ],
            ..S
        },
        ScriptTest {
            name: r#"2pk5cols-ints: add a row to a two primary table using dolt table put-row"#,
            intent: r#"A row added to a committed table with a two-column key shows in the diff by both key values."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2)); COMMENT ON COLUMN test.pk1 IS 'tag:0'; COMMENT ON COLUMN test.pk2 IS 'tag:1'; COMMENT ON COLUMN test.c1 IS 'tag:2'; COMMENT ON COLUMN test.c2 IS 'tag:3'; COMMENT ON COLUMN test.c3 IS 'tag:4'; COMMENT ON COLUMN test.c4 IS 'tag:5'; COMMENT ON COLUMN test.c5 IS 'tag:6';"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added test table');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT to_pk1, to_pk2, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_pk1", INT8), Column("to_pk2", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"2pk5cols-ints: add a row where one of the primary keys is different, not both"#,
            intent: r#"Rows that share one key column but differ in the other are distinct rows."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2)); COMMENT ON COLUMN test.pk1 IS 'tag:0'; COMMENT ON COLUMN test.pk2 IS 'tag:1'; COMMENT ON COLUMN test.c1 IS 'tag:2'; COMMENT ON COLUMN test.c2 IS 'tag:3'; COMMENT ON COLUMN test.c3 IS 'tag:4'; COMMENT ON COLUMN test.c4 IS 'tag:5'; COMMENT ON COLUMN test.c5 IS 'tag:6';"#,
                r#"INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0, 1, 1, 2, 3, 4, 10);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("0"), T("1"), T("1"), T("2"), T("3"), T("4"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"2pk5cols-ints: overwrite a row with two primary keys"#,
            intent: r#"Writing a row with both key values of an existing row replaces it."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2)); COMMENT ON COLUMN test.pk1 IS 'tag:0'; COMMENT ON COLUMN test.pk2 IS 'tag:1'; COMMENT ON COLUMN test.c1 IS 'tag:2'; COMMENT ON COLUMN test.c2 IS 'tag:3'; COMMENT ON COLUMN test.c3 IS 'tag:4'; COMMENT ON COLUMN test.c4 IS 'tag:5'; COMMENT ON COLUMN test.c5 IS 'tag:6';"#,
                r#"INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 5);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0, 0, 1, 2, 3, 4, 10) ON CONFLICT (pk1, pk2) DO UPDATE SET c1 = excluded.c1, c2 = excluded.c2, c3 = excluded.c3, c4 = excluded.c4, c5 = excluded.c5;"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's REPLACE INTO is Postgres' INSERT ... ON CONFLICT DO UPDATE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("1"), T("2"), T("3"), T("4"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"2pk5cols-ints: interact with a multiple primary key table with sql"#,
            intent: r#"Rows of a table with a two-column key insert, select by one key column, refuse a repeated key, and need both key columns."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2)); COMMENT ON COLUMN test.pk1 IS 'tag:0'; COMMENT ON COLUMN test.pk2 IS 'tag:1'; COMMENT ON COLUMN test.c1 IS 'tag:2'; COMMENT ON COLUMN test.c2 IS 'tag:3'; COMMENT ON COLUMN test.c3 IS 'tag:4'; COMMENT ON COLUMN test.c4 IS 'tag:5'; COMMENT ON COLUMN test.c5 IS 'tag:6';"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,0,6,6,6,6,6);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("6"), T("6"), T("6"), T("6"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,1,7,7,7,7,7),(1,0,8,8,8,8,8);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("6"), T("6"), T("6"), T("6"), T("6")],
                            &[T("0"), T("1"), T("7"), T("7"), T("7"), T("7"), T("7")],
                            &[T("1"), T("0"), T("8"), T("8"), T("8"), T("8"), T("8")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test WHERE pk1=1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("pk2", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("1"), T("0"), T("8"), T("8"), T("8"), T("8"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk1,pk2,c1,c2,c3,c4,c5) VALUES (0,1,7,7,7,7,7);"#,
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "test_pkey""#, detail: "Key (pk1, pk2)=(0, 1) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (pk1,c1,c2,c3,c4,c5) VALUES (0,6,6,6,6,6);"#,
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "pk2" of relation "test" violates not-null constraint"#, detail: "Failing row contains (0, null, 6, 6, 6, 6, 6).", schema: "public", table: "test", column: "pk2", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's error for a missing value of a column with no default is Postgres' not-null violation")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test (c1,c2,c3,c4,c5) VALUES (6,6,6,6,6);"#,
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "pk1" of relation "test" violates not-null constraint"#, detail: "Failing row contains (null, null, 6, 6, 6, 6, 6).", schema: "public", table: "test", column: "pk1", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's error for a missing value of a column with no default is Postgres' not-null violation")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
