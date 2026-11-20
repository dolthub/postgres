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

//! Dolt's integration-tests/bats/sql-load-data.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};

#[test]
fn test_sql_load_data() {
    run_scripts(&[
        ScriptTest {
            name: "sql-load-data: simple load from file into table",
            intent: "A delimited file with a header line loads its rows.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int, c2 int, c3 int, c4 int, c5 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN (FORMAT csv, DELIMITER '|', HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-ints.psv",
                    dolt: Some(Dolt::MySqlOnly("LOAD DATA INFILE becomes COPY FROM STDIN, whose delimiter is one character, so the file separates its fields with | where Dolt's uses ||")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4), Column("c3", INT4), Column("c4", INT4), Column("c5", INT4)],
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
            name: "sql-load-data: load into unknown table throws error",
            intent: "Loading into a missing table fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN (FORMAT csv, DELIMITER '|', HEADER);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"test\" does not exist", ..E }),
                    copy_from_stdin_file: "bats-load-data-ints.psv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: load with unknown file throws error",
            intent: "Loading from a missing server file fails.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int, c2 int, c3 int, c4 int, c5 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM 'hello-ints.csv' (FORMAT csv, DELIMITER '|', HEADER);",
                    expected: Expected::Error(Diagnostic { code: "58P01", message: "could not open file \"hello-ints.csv\" for reading: No such file or directory", hint: "COPY FROM instructs the PostgreSQL server process to read a file. You may want a client-side facility such as psql's \\copy.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: works with enclosed terms",
            intent: "Quoted fields load without their quotes.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int, c2 int, c3 int, c4 int, c5 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN (FORMAT csv, DELIMITER '|', QUOTE '\"', HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-enclosed.psv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4), Column("c3", INT4), Column("c4", INT4), Column("c5", INT4)],
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
            name: "sql-load-data: works when the number of input columns in the file is less than the number of schema columns",
            intent: "Columns the file leaves out load as NULL.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int, c2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test (pk, c1) FROM STDIN (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-fewer.csv",
                    dolt: Some(Dolt::Differs("Postgres' COPY refuses a line that is missing fields, where LOAD DATA loads NULL into the missing columns, so the port names the file's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("1"), Null],
                            &[T("1"), T("1"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: works with fields separated by tabs",
            intent: "Tab-separated fields load.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN (HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-tabs.tsv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("1")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: recognizes certain nulls",
            intent: "The null marker loads as NULL.",
            set_up_script: &[
                "CREATE TABLE test (pk text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN (HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-nulls.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM test WHERE pk IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres' COPY reads only \\N as NULL, so the word NULL loads as text, where LOAD DATA also reads it as NULL")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: works when column order is mismatched",
            intent: "A column list maps the file's fields to columns in its own order.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test (c1, pk) FROM STDIN (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-mismatched.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("1"), T("hi")],
                            &[T("2"), T("hello")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: with different column types that uses optionally",
            intent: "Quoted fields may hold the delimiter and escaped quotes.",
            set_up_script: &[
                "CREATE TABLE test (pk int, c1 text, c2 float8);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN (FORMAT csv, ESCAPE '\\');",
                    expected: Expected::Tag("COPY 4"),
                    copy_from_stdin_file: "bats-load-data-optionally.csv",
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"This functionality is not present yet.\"), and its 100.20 for every row is a typo for the file's values; COPY's CSV format reads OPTIONALLY ENCLOSED BY's quotes, with ESCAPE for its backslashes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT), Column("c2", FLOAT8)],
                        rows: &[
                            &[T("1"), T("a string"), T("100.2")],
                            &[T("2"), T("a string containing a , comma"), T("102.2")],
                            &[T("3"), T("a string containing a \" quote"), T("102.2")],
                            &[T("4"), T("a string containing a \", quote and comma"), T("102.2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: works with escaped columns",
            intent: "Backslash escapes in a loaded field are decoded.",
            set_up_script: &[
                "CREATE TABLE loadtable (pk text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY loadtable FROM STDIN;",
                    expected: Expected::Tag("COPY 4"),
                    copy_from_stdin_file: "bats-load-data-escaped.txt",
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"This functionality is not present yet.\"), and it loads ./testdata/test5.txt instead of the file it writes; COPY's text format decodes the backslash escapes, and has no quotes to enclose fields in")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM loadtable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", TEXT)],
                        rows: &[
                            &[T("hi")],
                            &[T("hello")],
                            &[T("TryN")],
                            &[T("new\ns")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: when the number of input columns in the file is greater than the number of schema columns",
            intent: "A line with more fields than the table has columns is refused.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c1 int, c2 int, c3 int, c4 int, c5 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN (FORMAT csv, DELIMITER '|', HEADER);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "extra data after last expected column", ..E }),
                    copy_from_stdin_file: "bats-load-data-greater.psv",
                    dolt: Some(Dolt::Differs("Postgres' COPY refuses a line with extra fields, where LOAD DATA drops them; Dolt skips this test (\"This functionality is not present yet.\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4), Column("c3", INT4), Column("c4", INT4), Column("c5", INT4)],
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
            name: "sql-load-data: run twice it appends",
            intent: "Loading again appends rows, and a duplicate key fails the load.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-zeros.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-ones.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    copy_from_stdin_file: "bats-load-data-ones.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: load data ignore",
            intent: "Loaded rows whose keys exist are skipped.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "INSERT INTO t VALUES (0, 0, 0);",
                "CREATE TABLE staging (pk int, c1 int, c2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY staging FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-rows.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t SELECT * FROM staging ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("COPY has no IGNORE, so the file loads into a staging table that INSERT ... ON CONFLICT DO NOTHING merges")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: load data replace",
            intent: "Loaded rows whose keys exist replace them.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
                "INSERT INTO t VALUES (0, 0, 0);",
                "CREATE TABLE staging (pk int, c1 int, c2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY staging FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-replace.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t SELECT * FROM staging ON CONFLICT (pk) DO UPDATE SET c1 = excluded.c1, c2 = excluded.c2;",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("COPY has no REPLACE, so the file loads into a staging table that INSERT ... ON CONFLICT DO UPDATE merges")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("1")],
                            &[T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: keyless table",
            intent: "A table without a primary key takes the same rows again.",
            set_up_script: &[
                "CREATE TABLE t (c1 varchar(10), c2 varchar(20), c3 varchar(30));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-keyless.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR), Column("c2", VARCHAR), Column("c3", VARCHAR)],
                        rows: &[
                            &[T("this"), T("is"), T("keyless")],
                            &[T("and"), T("uses"), T("strings")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-keyless.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(c1) FROM t WHERE c1 = 'this';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: schema with string and numerical types",
            intent: "Fields load into string, number, and enum columns, and an out of range number fails.",
            set_up_script: &[
                "CREATE TYPE c7_enum AS ENUM ('a', 'b');",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 char(1), c2 varchar(1), c3 text, c4 int, c5 smallint, c6 float8, c7 c7_enum);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-types-1.csv",
                    dolt: Some(Dolt::MySqlOnly("tinyint becomes smallint, double becomes float8, and the enum column a type made with CREATE TYPE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", BPCHAR), Column("c2", VARCHAR), Column("c3", TEXT), Column("c4", INT4), Column("c5", INT2), Column("c6", FLOAT8), Column("c7", USER_DEFINED)],
                        rows: &[
                            &[T("0"), T("a"), T("a"), T("this is text"), T("0"), T("0"), T("0.01"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "value \"5555555\" is out of range for type smallint", ..E }),
                    copy_from_stdin_file: "bats-load-data-types-2.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: date types",
            intent: "Fields load into date and time columns, and an invalid timestamp fails.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 timestamp, c2 date, c3 time);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-dates-1.csv",
                    dolt: Some(Dolt::MySqlOnly("datetime becomes timestamp")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TIMESTAMP), Column("c2", DATE), Column("c3", TIME)],
                        rows: &[
                            &[T("0"), T("2022-10-10 00:00:00"), T("2022-10-10"), T("00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Error(Diagnostic { code: "22007", message: "invalid input syntax for type timestamp: \"thisisnotavaliddate\"", ..E }),
                    copy_from_stdin_file: "bats-load-data-dates-2.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: schema with not null constraints",
            intent: "A NULL for a NOT NULL column fails the load and keeps the earlier rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int NOT NULL, c2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-zeros.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (DELIMITER ',');",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"c1\" of relation \"t\" violates not-null constraint", detail: "Failing row contains (1, null, 1).", schema: "public", table: "t", column: "c1", ..E }),
                    copy_from_stdin_file: "bats-load-data-null-c1.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
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
            name: "sql-load-data: test schema with column defaults",
            intent: "An explicit NULL in a loaded file is stored rather than the column default.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int DEFAULT 1, c2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-zeros.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (DELIMITER ',');",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-null-c1.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), Null, T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Bug("Dolt expects a NUL byte in an int column for the loaded NULL, where the loaded NULL is stored")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: test schema with check constraints",
            intent: "A row failing a check constraint fails the load, and a passing row loads.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int, CHECK (c1 > 0));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t\" violates check constraint \"t_c1_check\"", detail: "Failing row contains (0, 0, 0).", schema: "public", table: "t", constraint: "t_c1_check", ..E }),
                    copy_from_stdin_file: "bats-load-data-zeros.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-check.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: test schema with foreign keys",
            intent: "A row without its parent fails the load, unless foreign key checks are off.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c1 int, c2 int);",
                "CREATE TABLE t2 (pk int PRIMARY KEY, FOREIGN KEY (pk) REFERENCES t1 (pk));",
                "INSERT INTO t1 VALUES (0, 0, 0), (2, 2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t2 FROM STDIN;",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-fk-1.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t2 FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"t2\" violates foreign key constraint \"t2_pk_fkey\"", detail: "Key (pk)=(1) is not present in table \"t1\".", schema: "public", table: "t2", constraint: "t2_pk_fkey", ..E }),
                    copy_from_stdin_file: "bats-load-data-fk-2.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("foreign_key_checks is MySQL's, and session_replication_role = replica turns off Postgres' foreign key checks")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t2 FROM STDIN;",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "bats-load-data-fk-2.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-load-data: sql-server mode",
            intent: "The server refuses a missing file and loads a file its client sends.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int, c2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t FROM 'foo.csv';",
                    expected: Expected::Error(Diagnostic { code: "58P01", message: "could not open file \"foo.csv\" for reading: No such file or directory", hint: "COPY FROM instructs the PostgreSQL server process to read a file. You may want a client-side facility such as psql's \\copy.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-load-data-rows.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("1"), T("1")],
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
