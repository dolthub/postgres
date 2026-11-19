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

//! Dolt's integration-tests/bats/import-replace-tables.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_import_replace_tables() {
    run_scripts(&[
        ScriptTest {
            name: "import-replace-tables: replace table using csv",
            intent: "Replacing a table's rows with a CSV file's rows loads them.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test;",
                    expected: Expected::Tag("DELETE 0"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -r is the CLI's, so DELETE empties the table and COPY ... FROM STDIN loads the file")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-1pk5col-ints.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("1"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-replace-tables: replace table using psv",
            intent: "Replacing a table's rows with a pipe-separated file's rows loads them.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test;",
                    expected: Expected::Tag("DELETE 0"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -r is the CLI's, so DELETE empties the table and COPY ... FROM STDIN loads the file")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER, DELIMITER '|');",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-import-replace-1pk5col-ints.psv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("1"), T("1"), T("2"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-replace-tables: replace table with 2 primary keys with a csv with 2 primary keys",
            intent: "Replacing a two-key table's rows with the same file leaves the file's rows.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk1 int NOT NULL, pk2 int NOT NULL, c1 int, c2 int, c3 int, c4 int, c5 int, PRIMARY KEY (pk1, pk2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE makes the table with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 4"),
                    copy_from_stdin_file: "bats-2pk5col-ints.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test;",
                    expected: Expected::Tag("DELETE 4"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -r is the CLI's, so DELETE empties the table and COPY ... FROM STDIN loads the file")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 4"),
                    copy_from_stdin_file: "bats-2pk5col-ints.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
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
            name: "import-replace-tables: replacing a table with a subset of data correctly works",
            intent: "Replacing a table's rows with rows naming some columns leaves the others NULL.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int, c1 int, c2 int, c3 int, c4 int, c5 int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0,1,2,3,4,5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM test;",
                    expected: Expected::Tag("DELETE 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -r is the CLI's, so DELETE empties the table and INSERT writes the file's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c2) VALUES (0, 7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4), Column("c3", INT4), Column("c4", INT4), Column("c5", INT4)],
                        rows: &[
                            &[T("0"), Null, T("7"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-replace-tables: Replace that breaks fk constraints correctly errors",
            intent: "Emptying a table whose rows a foreign key references fails and keeps the rows.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX color_index ON colors (color);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so color_index is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM colors;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "update or delete on table \"colors\" violates foreign key constraint \"objects_color_fkey\" on table \"objects\"", detail: "Key (color)=(red) is still referenced from table \"objects\".", schema: "public", table: "objects", constraint: "objects_color_fkey", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -r is the CLI's, so DELETE empties the table, which its file's rows would refill")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM colors;",
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
    ]);
}
