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

//! Dolt's integration-tests/bats/export-tables.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Dolt, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_export_tables() {
    run_scripts(&[
        ScriptTest {
            name: "export-tables: dolt table import from stdin export to stdout",
            intent: "Rows loaded from a client's file export to it in CSV.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test_int FROM STDIN (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "bats-export-stdin.csv",
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u and dolt table export to stdout are the CLI's, so COPY FROM STDIN and COPY TO STDOUT move the rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test_int TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_to_stdout_file: "bats-export-stdin.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "export-tables: dolt table export",
            intent: "A table exports its header and rows as CSV.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
                "INSERT INTO test_int VALUES (0, 1, 2, 3, 4, 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test_int TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "bats-export-int.csv",
                    dolt: Some(Dolt::MySqlOnly("dolt table export's refusal to overwrite a file and its -f flag are the CLI's, so COPY TO STDOUT checks the exported rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "export-tables: export a table with a string with commas to csv",
            intent: "A CSV export quotes a field holding commas.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test_string VALUES ('tim', 'is', 'super', 'duper', 'rad', 'a,b,c,d,e');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test_string TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "bats-export-commas.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "export-tables: export a table with a string with double quotes to csv",
            intent: "A CSV export doubles the quotes inside a quoted field.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test_string (pk, c1, c5) VALUES ('this', 'is', 'a \"quotation\"');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test_string TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "bats-export-quotes.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "export-tables: export a table with a string with new lines to csv",
            intent: "A CSV export quotes a field holding a newline, and its bytes survive a reimport.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test_string (pk, c1, c5) VALUES ('this', 'is', E'a new \\n line');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test_string TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "bats-export-newlines.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE newline_export (a int PRIMARY KEY, b bytea);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("VARBINARY becomes bytea, LOCATE(CHAR(10), b) becomes position, and the export and import -c become a COPY round trip into a copy of the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO newline_export VALUES (1, E'line\\nbreak'::bytea), (2, E'sorry\\ncsv'::bytea);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE newline_copy (a int PRIMARY KEY, b bytea);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY newline_export TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_round_trip_stdin_query: "COPY newline_copy FROM STDIN (FORMAT csv, HEADER);",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, length(b) AS byte_length, position('\\x0a'::bytea IN b) AS newline_position FROM newline_copy ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("byte_length", INT4), Column("newline_position", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("5")],
                            &[T("2"), T("9"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "export-tables: table with column with not null constraint can be exported and reimported",
            intent: "Empty strings in NOT NULL columns, and empty values apart from NULLs, survive an export and reimport.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
                "CREATE TABLE person_info (name varchar(255) NOT NULL, location varchar(255) NOT NULL, age bigint NOT NULL, PRIMARY KEY (name));",
                "CREATE TABLE person_copy (name varchar(255) NOT NULL, location varchar(255) NOT NULL, age bigint NOT NULL, PRIMARY KEY (name));",
                "INSERT INTO person_info (name, location, age) VALUES ('kevern smith', 'los angeles', 21);",
                "INSERT INTO person_info (name, location, age) VALUES ('barbara smith', 'los angeles', 24);",
                "INSERT INTO person_info (name, location, age) VALUES ('gary busy', '', 900);",
                "INSERT INTO person_info (name, location, age) VALUES ('the tampa bay buccs', 'florida', 123);",
                "INSERT INTO person_info (name, location, age) VALUES ('michael fakeperson', 'fake city', 39);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY person_info TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 5"),
                    copy_round_trip_stdin_query: "COPY person_copy FROM STDIN (FORMAT csv, HEADER);",
                    dolt: Some(Dolt::MySqlOnly("the CLI's export, checkout, and import -u become a COPY round trip into a copy of the table, VARBINARY becomes bytea, and IF becomes CASE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM person_copy ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", VARCHAR), Column("location", VARCHAR), Column("age", INT8)],
                        rows: &[
                            &[T("barbara smith"), T("los angeles"), T("24")],
                            &[T("gary busy"), T(""), T("900")],
                            &[T("kevern smith"), T("los angeles"), T("21")],
                            &[T("michael fakeperson"), T("fake city"), T("39")],
                            &[T("the tampa bay buccs"), T("florida"), T("123")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE empty_string_export (pk int PRIMARY KEY, v bytea);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO empty_string_export VALUES (1, ''), (2, NULL);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE empty_string_copy (pk int PRIMARY KEY, v bytea);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY empty_string_export TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_round_trip_stdin_query: "COPY empty_string_copy FROM STDIN (FORMAT csv, HEADER);",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, CASE WHEN v IS NULL THEN 1 ELSE 0 END AS is_null FROM empty_string_copy ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("is_null", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "export-tables: export a table with a json string to csv",
            intent: "A CSV export quotes a JSON value and doubles its quotes.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
                "CREATE TABLE t2 (id int PRIMARY KEY, j json);",
                "INSERT INTO t2 VALUES (0, '[\"hi\"]');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t2 TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "bats-export-json.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "export-tables: uint schema parsing for writer_test.go backwards compatibility",
            intent: "A table without a primary key exports its rows as CSV.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
                "CREATE TABLE t2 (name text, age bigint, title text);",
                "INSERT INTO t2 VALUES ('Bill Billerson', 32, 'Senior Dufus');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY t2 TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    copy_to_stdout_file: "bats-export-uint.csv",
                    dolt: Some(Dolt::MySqlOnly("int unsigned becomes bigint")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "export-tables: exporting a table with datetimes can be reimported",
            intent: "Timestamps survive an export and reimport.",
            set_up_script: &[
                "CREATE TABLE test_int (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                "CREATE TABLE test_string (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
                "CREATE TABLE timetable (pk int PRIMARY KEY, time timestamp);",
                "INSERT INTO timetable VALUES (1, '2021-06-02 15:37:24');",
                "CREATE TABLE timetable_copy (pk int PRIMARY KEY, time timestamp);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY timetable TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 1"),
                    copy_round_trip_stdin_query: "COPY timetable_copy FROM STDIN (FORMAT csv, HEADER);",
                    dolt: Some(Dolt::MySqlOnly("datetime becomes timestamp, and the CLI's export, table rm, and import -c become a COPY round trip into a copy of the table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM timetable_copy;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("time", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("2021-06-02 15:37:24")],
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
