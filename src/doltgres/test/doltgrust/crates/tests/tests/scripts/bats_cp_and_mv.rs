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

//! Dolt's integration-tests/bats/cp-and-mv.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_cp_and_mv() {
    run_scripts(&[
        ScriptTest {
            name: r#"cp-and-mv: cp table"#,
            intent: r#"Copying a table makes a new table with its rows."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test_new (LIKE test1 INCLUDING ALL);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table cp is a CLI command that copies a table's definition and rows, which SQL does with CREATE TABLE ... (LIKE ... INCLUDING ALL) and INSERT ... SELECT"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test_new SELECT * FROM test1;"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"test1"#)],
                            &[T(r#"test2"#)],
                            &[T(r#"test_new"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test_new;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: mv table"#,
            intent: r#"Renaming a table keeps its rows under the new name."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test1 RENAME TO test_new;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table mv is a CLI command, which SQL does with ALTER TABLE ... RENAME TO"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"test2"#)],
                            &[T(r#"test_new"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test_new;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: cp table with the existing name"#,
            intent: r#"Copying a table onto an existing name fails."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test2 (LIKE test1 INCLUDING ALL);"#,
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "test2" already exists"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table cp is a CLI command that copies a table's definition and rows, which SQL does with CREATE TABLE ... (LIKE ... INCLUDING ALL) and INSERT ... SELECT"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: mv table with the existing name"#,
            intent: r#"Renaming a table onto an existing name fails."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test1 RENAME TO test2;"#,
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "test2" already exists"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table mv is a CLI command, which SQL does with ALTER TABLE ... RENAME TO"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: cp nonexistent table"#,
            intent: r#"Copying a table that doesn't exist fails."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test3 (LIKE not_found INCLUDING ALL);"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "not_found" does not exist"#, position: 26, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table cp is a CLI command that copies a table's definition and rows, which SQL does with CREATE TABLE ... (LIKE ... INCLUDING ALL) and INSERT ... SELECT"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: mv non_existent table"#,
            intent: r#"Renaming a table that doesn't exist fails."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE not_found RENAME TO test3;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "not_found" does not exist"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table mv is a CLI command, which SQL does with ALTER TABLE ... RENAME TO"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: cp table with invalid name"#,
            intent: r#"A copy can't take a name reserved for Dolt's system tables."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE dolt_docs (LIKE test1 INCLUDING ALL);"#,
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"Invalid table name dolt_docs. Table names beginning with `dolt_` are reserved for internal use"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table cp is a CLI command that copies a table's definition and rows, which SQL does with CREATE TABLE ... (LIKE ... INCLUDING ALL) and INSERT ... SELECT"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE dolt_query_catalog (LIKE test1 INCLUDING ALL);"#,
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"Invalid table name dolt_query_catalog. Table names beginning with `dolt_` are reserved for internal use"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table cp is a CLI command that copies a table's definition and rows, which SQL does with CREATE TABLE ... (LIKE ... INCLUDING ALL) and INSERT ... SELECT"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE dolt_reserved (LIKE test1 INCLUDING ALL);"#,
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"Invalid table name dolt_reserved. Table names beginning with `dolt_` are reserved for internal use"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table cp is a CLI command that copies a table's definition and rows, which SQL does with CREATE TABLE ... (LIKE ... INCLUDING ALL) and INSERT ... SELECT"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: mv table with invalid name"#,
            intent: r#"A rename can't take a name reserved for Dolt's system tables."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test1 RENAME TO dolt_docs;"#,
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"Invalid table name dolt_docs. Table names beginning with `dolt_` are reserved for internal use"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table mv is a CLI command, which SQL does with ALTER TABLE ... RENAME TO"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test1 RENAME TO dolt_query_catalog;"#,
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"Invalid table name dolt_query_catalog. Table names beginning with `dolt_` are reserved for internal use"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table mv is a CLI command, which SQL does with ALTER TABLE ... RENAME TO"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test1 RENAME TO dolt_reserved;"#,
                    expected: Expected::Error(Diagnostic { code: "42939", message: r#"Invalid table name dolt_reserved. Table names beginning with `dolt_` are reserved for internal use"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table mv is a CLI command, which SQL does with ALTER TABLE ... RENAME TO"#)),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: rm table"#,
            intent: r#"Dropping a table removes only it."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP TABLE test1;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table rm is a CLI command, which SQL does with DROP TABLE"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T(r#"test2"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"cp-and-mv: rm tables"#,
            intent: r#"Dropping two tables removes both."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP TABLE test1, test2;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table rm is a CLI command, which SQL does with DROP TABLE"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
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
            name: r#"cp-and-mv: rm nonexistent table"#,
            intent: r#"Dropping a table that doesn't exist fails."#,
            set_up_script: &[
                r#"CREATE TABLE test1 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test1 VALUES (1, 1);"#,
                r#"CREATE TABLE test2 (pk BIGINT NOT NULL, c1 BIGINT, PRIMARY KEY (pk));"#,
                r#"INSERT INTO test2 VALUES (2, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP TABLE abcdefz;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"table "abcdefz" does not exist"#, ..E }),
                    dolt: Some(Dolt::MySqlOnly(r#"dolt table rm is a CLI command, which SQL does with DROP TABLE"#)),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
