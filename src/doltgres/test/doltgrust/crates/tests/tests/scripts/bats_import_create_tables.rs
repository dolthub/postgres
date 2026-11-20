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

//! Dolt's integration-tests/bats/import-create-tables.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_import_create_tables() {
    run_scripts(&[
        ScriptTest {
            name: "import-create-tables: import csv with invalid utf8",
            intent: "A file with a byte that isn't UTF-8 fails to load.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (id int NOT NULL, name VARCHAR(255), PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t (id, name) FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"UTF8\": 0x85", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file into the empty table, and --continue's truncation at the bad byte is MySQL's and left out")),
                    copy_from_stdin_file: "bats-import-create-invalid-utf8.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t;",
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
            name: "import-create-tables: dolt diff on a newly created table",
            intent: "A created table shows as added in the working diff.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.test"), T("added"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-create-tables: fail on import table creation when defined pk has a NULL value",
            intent: "Loading a row with an empty primary key value fails, for one and two key columns.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk varchar(200) NOT NULL, v1 int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE makes the table with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test (pk, v1) FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"pk\" of relation \"test\" violates not-null constraint", detail: "Failing row contains (null, 2).", schema: "public", table: "test", column: "pk", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import is the CLI's, so COPY ... FROM STDIN loads the file into the table, naming the columns of its header")),
                    copy_from_stdin_file: "bats-import-create-null-pk-1.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk1 int NOT NULL, pk2 int NOT NULL, v1 int, PRIMARY KEY (pk1, pk2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE makes the table with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test (pk1, pk2, v1) FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"pk2\" of relation \"test\" violates not-null constraint", detail: "Failing row contains (1, null, 1).", schema: "public", table: "test", column: "pk2", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import is the CLI's, so COPY ... FROM STDIN loads the file into the table, naming the columns of its header")),
                    copy_from_stdin_file: "bats-import-create-null-pk-2.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-create-tables: csv files has less columns than -s schema",
            intent: "A load that leaves out a column leaves it NULL.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE subset (pk int NOT NULL, c1 int, c3 int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c -s is the CLI's, so CREATE TABLE makes the table from the schema file")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY subset (pk, c3) FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import is the CLI's, so COPY ... FROM STDIN loads the file into the table, naming the columns of its header")),
                    copy_from_stdin_file: "bats-import-create-subset.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM subset ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c3", INT4)],
                        rows: &[
                            &[T("0"), Null, T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-create-tables: csv files has equal columns but different order than -s schema",
            intent: "Columns named in another order load into the right columns.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE subset (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c -s is the CLI's, so CREATE TABLE makes the table from the schema file")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY subset (pk, c2, c1) FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import is the CLI's, so COPY ... FROM STDIN loads the file into the table, naming the columns of its header")),
                    copy_from_stdin_file: "bats-import-create-order.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM subset ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-create-tables: csv files has fewer columns filled with default value",
            intent: "A load that leaves out a column fills it with the column's default.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE subset (pk int NOT NULL, c1 int DEFAULT 42, c2 int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c -s is the CLI's, so CREATE TABLE makes the table from the schema file")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY subset (pk, c2) FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import is the CLI's, so COPY ... FROM STDIN loads the file into the table, naming the columns of its header")),
                    copy_from_stdin_file: "bats-import-create-default.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM subset ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("42"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-create-tables: keyless table import",
            intent: "Loading into a table without a primary key fills a left-out column with its default.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE keyless (c0 int, c1 int DEFAULT 42, c2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c -s is the CLI's, so CREATE TABLE makes the table from the schema file")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY keyless (c0, c2) FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import is the CLI's, so COPY ... FROM STDIN loads the file into the table, naming the columns of its header")),
                    copy_from_stdin_file: "bats-import-create-keyless.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("42"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-create-tables: auto-increment table",
            intent: "Rows with explicit identity keys load, and a later generated key follows the identity's own sequence.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, v1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c -s is the CLI's, so CREATE TABLE makes the table from the schema file, with MySQL's AUTO_INCREMENT as an identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test (pk, v1) FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 4"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import is the CLI's, so COPY ... FROM STDIN loads the file into the table, naming the columns of its header")),
                    copy_from_stdin_file: "bats-import-create-identity.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (DEFAULT, 5);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    dolt: Some(Dolt::Differs("Postgres' identity sequences never move back and never jump on their own: values continue after every value used on any branch sharing the table's storage, explicit keys don't move them, and RESTART WITH sets the next value exactly, where Dolt's AUTO_INCREMENT tracker recomputes from the rows the branches hold")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
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
            name: "import-create-tables: import null foreign key value does not violate constraint",
            intent: "A NULL foreign key value loads without a parent row.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE states (id int NOT NULL, abbr char(2), PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE data (id int NOT NULL, state_id int, data varchar(500), PRIMARY KEY (id), CONSTRAINT d4jibcjf FOREIGN KEY (state_id) REFERENCES states (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX state_id ON data (state_id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's KEY clause of CREATE TABLE is CREATE INDEX")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO data VALUES (0, NULL, 'poop');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM data;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("state_id", INT4), Column("data", VARCHAR)],
                        rows: &[
                            &[T("0"), Null, T("poop")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO data (id, state_id, data) VALUES (1, NULL, 'poop') ON CONFLICT (id) DO UPDATE SET state_id = EXCLUDED.state_id, data = EXCLUDED.data;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM data ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("state_id", INT4), Column("data", VARCHAR)],
                        rows: &[
                            &[T("0"), Null, T("poop")],
                            &[T("1"), Null, T("poop")],
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
