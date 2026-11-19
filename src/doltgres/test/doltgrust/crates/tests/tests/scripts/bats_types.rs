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

//! Dolt's integration-tests/bats/types.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};

#[test]
fn test_types() {
    run_scripts(&[
        ScriptTest {
            name: "types: sql engine can parse all numeric data literals",
            intent: "Numeric literals of every form parse.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, i bigint, ui numeric(20), f double precision, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's BIGINT UNSIGNED is numeric(20)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,i) VALUES (0,9223372036854775807);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,ui) VALUES (1,18446744073709551615);",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Bug("Dolt skips the rest of this test (\"We can't parse values above the INT64 max in go-mysql-server yet\"), where Postgres parses the literal")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,f) VALUES (2, 8.988465674311578540726371186585217839905e+307);",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Bug("Dolt skips the rest of this test (\"We can't parse large float values in go-mysql-server yet\"), where Postgres parses the literal")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk,f) VALUES (3, 170141173319264429905852091742258462720);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: BIGINT",
            intent: "A BIGINT column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("bigint")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 4611686018427387903);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT8)],
                        rows: &[
                            &[T("1"), T("4611686018427387903")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v=v*2+1 WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT8)],
                        rows: &[
                            &[T("1"), T("9223372036854775807")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 40000000000000000000);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "bigint out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, -40000000000000000000);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "bigint out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("bigint"), Null, T("64"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: BINARY(10)",
            intent: "A BINARY(10) column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v bytea, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("bytea")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abcdefg');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BYTEA)],
                        rows: &[
                            &[T("1"), T("\\x61626364656667")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v='1234567890' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BYTEA)],
                        rows: &[
                            &[T("1"), T("\\x31323334353637383930")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '12345678901');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres' bytea has no length limit, where MySQL's BINARY(10) refuses a longer value")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("bytea"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: BIT(10)",
            intent: "A BIT(10) column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v BIT(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("bit(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 511::bit(10));",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres assigns no integer to a bit column, so the values cast to bit(10) and back, and a cast keeps only the low ten bits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, v::integer FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("511")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres assigns no integer to a bit column, so the values cast to bit(10) and back, and a cast keeps only the low ten bits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v = (v::integer * 2 + 1)::bit(10) WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::Differs("Postgres assigns no integer to a bit column, so the values cast to bit(10) and back, and a cast keeps only the low ten bits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, v::integer FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1023")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres assigns no integer to a bit column, so the values cast to bit(10) and back, and a cast keeps only the low ten bits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 1024::bit(10));",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres assigns no integer to a bit column, so the values cast to bit(10) and back, and a cast keeps only the low ten bits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, (-1)::bit(10));",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres assigns no integer to a bit column, so the values cast to bit(10) and back, and a cast keeps only the low ten bits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("bit"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: BLOB",
            intent: "A BLOB column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v bytea, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("bytea")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abcdefg');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BYTEA)],
                        rows: &[
                            &[T("1"), T("\\x61626364656667")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres shows bytea in hex")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v='1234567890' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BYTEA)],
                        rows: &[
                            &[T("1"), T("\\x31323334353637383930")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres shows bytea in hex")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("bytea"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: BOOL",
            intent: "A BOOL column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v BOOL, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("boolean")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("boolean"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: BOOLEAN",
            intent: "A BOOLEAN column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v BOOLEAN, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("boolean")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, TRUE);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BOOL)],
                        rows: &[
                            &[T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, FALSE) ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BOOL)],
                        rows: &[
                            &[T("1"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("boolean"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: CHAR(10)",
            intent: "A CHAR(10) column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v CHAR(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("character(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abcdefg');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BPCHAR)],
                        rows: &[
                            &[T("1"), T("abcdefg   ")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v='1234567890' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BPCHAR)],
                        rows: &[
                            &[T("1"), T("1234567890")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '12345678901');",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character(10)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("character"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: CHARACTER(10)",
            intent: "A CHARACTER(10) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v CHARACTER(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("character(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("character"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: CHARACTER VARYING(10)",
            intent: "A CHARACTER VARYING(10) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v CHARACTER VARYING(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("character varying(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("character varying"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DATE",
            intent: "A DATE column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v DATE, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("date")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '2020-02-10 11:12:13.456789');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", DATE)],
                        rows: &[
                            &[T("1"), T("2020-02-10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-01-01 00:00:00') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", DATE)],
                        rows: &[
                            &[T("1"), T("1000-01-01")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-01-02') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", DATE)],
                        rows: &[
                            &[T("1"), T("1000-01-02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-01-3') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", DATE)],
                        rows: &[
                            &[T("1"), T("1000-01-03")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-1-04') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", DATE)],
                        rows: &[
                            &[T("1"), T("1000-01-04")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-1-5') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", DATE)],
                        rows: &[
                            &[T("1"), T("1000-01-05")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '9999-01-01 23:59:59.999999') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", DATE)],
                        rows: &[
                            &[T("1"), T("9999-01-01")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '99999-01-01 00:00:00');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres' dates reach far past year 9999, where MySQL's stop at it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '10000-01-01 00:00:00');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(2) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    dolt: Some(Dolt::Differs("the earlier out-of-range insert took key 2, which Postgres accepted, so this one fails on the key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("date"), Null, Null, Null, T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DATETIME",
            intent: "A DATETIME column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v timestamp(6), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("timestamp(6) without time zone")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '2020-02-10 11:12:13.456789');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("2020-02-10 11:12:13.456789")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-01-01 00:00:00') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("1000-01-01 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-01-02') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("1000-01-02 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-01-3') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("1000-01-03 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-1-04') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("1000-01-04 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1000-1-5') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("1000-01-05 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '9999-01-01 23:59:59.999999') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("9999-01-01 23:59:59.999999")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '99999-01-01 00:00:00');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres' dates reach far past year 9999, where MySQL's stop at it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '10000-01-01 00:00:00');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(2) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    dolt: Some(Dolt::Differs("the earlier out-of-range insert took key 2, which Postgres accepted, so this one fails on the key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("timestamp without time zone"), Null, Null, Null, T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v timestamp, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("timestamp without time zone")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '2020-02-10 11:12:13.456789');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("2020-02-10 11:12:13.456789")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DEC",
            intent: "A DEC column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v DEC, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DEC(9)",
            intent: "A DEC(9) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v DEC(9), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric(9,0)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, T("9"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DEC(9,5)",
            intent: "A DEC(9,5) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v DEC(9,5), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric(9,5)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, T("9"), T("5"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DECIMAL",
            intent: "A DECIMAL column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v DECIMAL, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DECIMAL(9)",
            intent: "A DECIMAL(9) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v DECIMAL(9), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric(9,0)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, T("9"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DECIMAL(9,5)",
            intent: "A DECIMAL(9,5) column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v DECIMAL(9,5), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric(9,5)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1234.56789);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", NUMERIC)],
                        rows: &[
                            &[T("1"), T("1234.56789")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v=v*2 WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", NUMERIC)],
                        rows: &[
                            &[T("1"), T("2469.13578")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 10000);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "numeric field overflow", detail: "A field with precision 9, scale 5 must round to an absolute value less than 10^4.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, T("9"), T("5"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DOUBLE",
            intent: "A DOUBLE column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v double precision, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("double precision")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1.25);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", FLOAT8)],
                        rows: &[
                            &[T("0"), T("1.25")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", FLOAT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", FLOAT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("double precision"), Null, T("53"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: DOUBLE PRECISION",
            intent: "A DOUBLE PRECISION column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v DOUBLE PRECISION, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("double precision")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("double precision"), Null, T("53"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: ENUM('a','b','c')",
            intent: "A ENUM('a','b','c') column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TYPE test_v AS ENUM ('a', 'b', 'c');",
                    expected: Expected::Tag("CREATE TYPE"),
                    dolt: Some(Dolt::Differs("Postgres' enum is a type of its own, made before the table, where MySQL lists the values in the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v test_v, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("test_v")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v='b' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's enum value 2 is its second listed value, 'b'")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 'd');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input value for enum test_v: \"d\"", position: 29, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input value for enum test_v: \"\"", position: 29, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("USER-DEFINED"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: FIXED",
            intent: "A FIXED column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v FIXED, PRIMARY KEY (pk));",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"fixed\" does not exist", position: 42, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test\" does not exist", position: 76, ..E }),
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: FIXED(9)",
            intent: "A FIXED(9) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v FIXED(9), PRIMARY KEY (pk));",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"fixed\" does not exist", position: 42, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test\" does not exist", position: 76, ..E }),
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: FIXED(9,5)",
            intent: "A FIXED(9,5) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v FIXED(9,5), PRIMARY KEY (pk));",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"fixed\" does not exist", position: 42, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test\" does not exist", position: 76, ..E }),
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: FLOAT",
            intent: "A FLOAT column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v FLOAT, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("double precision")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0, 1.25);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=0;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", FLOAT8)],
                        rows: &[
                            &[T("0"), T("1.25")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", FLOAT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", FLOAT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE pk=2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", FLOAT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("double precision"), Null, T("53"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: INT",
            intent: "A INT column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1073741823);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1073741823")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v=v*2+1 WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("2147483647")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2147483648);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "integer out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, -2147483649);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "integer out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("integer"), Null, T("32"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: INTEGER",
            intent: "A INTEGER column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v integer, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("integer"), Null, T("32"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NATIONAL CHAR(10)",
            intent: "A NATIONAL CHAR(10) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NATIONAL CHAR(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("character(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("character"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NATIONAL CHARACTER(10)",
            intent: "A NATIONAL CHARACTER(10) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NATIONAL CHARACTER(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("character(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("character"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NATIONAL CHARACTER VARYING(10)",
            intent: "A NATIONAL CHARACTER VARYING(10) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NATIONAL CHARACTER VARYING(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("character varying(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("character varying"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NATIONAL VARCHAR(10)",
            intent: "A NATIONAL VARCHAR(10) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NATIONAL VARCHAR(10), PRIMARY KEY (pk));",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"VARCHAR\"", position: 51, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test\" does not exist", position: 76, ..E }),
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NCHAR(10)",
            intent: "A NCHAR(10) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NCHAR(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("character(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("character"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NVARCHAR(10)",
            intent: "A NVARCHAR(10) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NVARCHAR(10), PRIMARY KEY (pk));",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"nvarchar\" does not exist", position: 42, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test\" does not exist", position: 76, ..E }),
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NUMERIC",
            intent: "A NUMERIC column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NUMERIC, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NUMERIC(9)",
            intent: "A NUMERIC(9) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NUMERIC(9), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric(9,0)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, T("9"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: NUMERIC(9,5)",
            intent: "A NUMERIC(9,5) column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v NUMERIC(9,5), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("numeric(9,5)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("numeric"), Null, T("9"), T("5"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: REAL",
            intent: "A REAL column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v REAL, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("real")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("real"), Null, T("24"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: SMALLINT",
            intent: "A SMALLINT column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v smallint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("smallint")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 16383);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT2)],
                        rows: &[
                            &[T("1"), T("16383")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v=v*2+1 WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", INT2)],
                        rows: &[
                            &[T("1"), T("32767")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 32768);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "smallint out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, -32769);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "smallint out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("smallint"), Null, T("16"), T("0"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: TEXT",
            intent: "A TEXT column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v TEXT, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("text")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abcdefg');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("abcdefg")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v='1234567890' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("1234567890")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("text"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: TIME",
            intent: "A TIME column stores and updates its values.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v TIME(6), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("time(6) without time zone")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '11:22:33.444444');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIME)],
                        rows: &[
                            &[T("1"), T("11:22:33.444444")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v='11:22' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIME)],
                        rows: &[
                            &[T("1"), T("11:22:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '838:59:59') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Error(Diagnostic { code: "22008", message: "date/time field value out of range: \"838:59:59\"", position: 29, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIME)],
                        rows: &[
                            &[T("1"), T("11:22:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '-838:59:59') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Error(Diagnostic { code: "22009", message: "time zone displacement out of range: \"-838:59:59\"", position: 29, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIME)],
                        rows: &[
                            &[T("1"), T("11:22:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("time without time zone"), Null, Null, Null, T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: TIMESTAMP",
            intent: "A TIMESTAMP column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v TIMESTAMP(6), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("timestamp(6) without time zone")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '2020-02-10 11:12:13.456789');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("2020-02-10 11:12:13.456789")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '1970-01-01 00:00:01') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("1970-01-01 00:00:01")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '2038-01-19 03:14:07.999999') ON CONFLICT (\"pk\") DO UPDATE SET \"v\" = EXCLUDED.\"v\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("2038-01-19 03:14:07.999999")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '1970-01-01 00:00:00');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres' timestamp has no 1970 to 2038 range, where MySQL's TIMESTAMP does")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '2038-01-19 03:14:08');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(2) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    dolt: Some(Dolt::Differs("the earlier out-of-range insert took key 2, which Postgres accepted, so this one fails on the key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("timestamp without time zone"), Null, Null, Null, T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE test;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v TIMESTAMP, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("timestamp without time zone")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, '2020-02-10 11:12:13.456789');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("2020-02-10 11:12:13.456789")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: VARBINARY(10)",
            intent: "A VARBINARY(10) column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v bytea, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("bytea")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abcdefg');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BYTEA)],
                        rows: &[
                            &[T("1"), T("\\x61626364656667")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v='1234567890' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", BYTEA)],
                        rows: &[
                            &[T("1"), T("\\x31323334353637383930")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '12345678901');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres' bytea has no length limit, where MySQL's VARBINARY(10) refuses a longer value")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("bytea"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types: VARCHAR(10)",
            intent: "A VARCHAR(10) column stores and updates values within its range and refuses values past it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, v VARCHAR(10), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'test'::regclass AND attname = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("format_type", TEXT)],
                        rows: &[
                            &[T("character varying(10)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so format_type shows the column's type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 'abcdefg');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("abcdefg")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET v='1234567890' WHERE pk=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1234567890")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, '12345678901');",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character varying(10)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data_type, character_maximum_length, numeric_precision, numeric_scale, datetime_precision FROM information_schema.columns WHERE table_name = 'test' AND column_name = 'v';",
                    expected: Expected::Rows {
                        columns: &[Column("data_type", VARCHAR), Column("character_maximum_length", INT4), Column("numeric_precision", INT4), Column("numeric_scale", INT4), Column("datetime_precision", INT4)],
                        rows: &[
                            &[T("character varying"), T("10"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' values for the column")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
